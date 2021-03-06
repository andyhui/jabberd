/*
 * Copyrights
 * 
 * Portions created by or assigned to Jabber.com, Inc. are 
 * Copyright (c) 1999-2002 Jabber.com, Inc.  All Rights Reserved.  Contact
 * information for Jabber.com, Inc. is available at http://www.jabber.com/.
 *
 * Portions Copyright (c) 1998-1999 Jeremie Miller.
 *
 * Portions Copyright (c) 2006-2007 Matthias Wimmer
 *
 * This file is part of jabberd14.
 *
 * This software is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include "jabberd.h"

/**
 * @file base_connect.cc
 * @brief connects to another instance using the component protocol
 *
 * This base handler implements connecting to another jabberd or other server instance
 * using the component protocol defined in XEP-0114.
 */

/* ---------------------------------------------------------
   base_connect - Connects to a specified host/port and 
                  exchanges xmlnodes with it over a socket
   ---------------------------------------------------------

   USAGE:
     <connect>
        <ip>1.2.3.4</ip>
	    <port>2020</port>
	    <secret>foobar</secret>
	    <timeout>5</timeout>
        <tries>15</tries>
     </connect>

   TODO: 
   - Add packet aging/delivery heartbeat
*/

/* Connection states */
typedef enum { conn_DIE, conn_CLOSED, conn_OPEN, conn_AUTHD } conn_state;

/* conn_info - stores thread data for a connection */
typedef struct {
    mio           io;
    conn_state    state;	/* Connection status (closed, opened, auth'd) */
    char*         hostip;	/* Server IP */
    u_short       hostport;	/* Server port */
    char*         secret;	/* Connection secret */
    int           timeout;      /* timeout for connect() in seconds */
    int           tries_left;   /* how many more times are we going to try to connect? */
    pool          mempool;	/* Memory pool for this struct */
    instance      inst;         /* Matching instance for this connection */
    pth_msgport_t   write_queue;	/* Queue of write_buf packets which need to be written */
    dpacket dplast; /* special circular reference detector */
} *conn_info, _conn_info;

/* conn_write_buf - stores a dpacket that needs to be written to the socket */
typedef struct {
    pth_message_t head;
    dpacket     packet;
} *conn_write_buf, _conn_write_buf;

static void base_connect_process_xml(mio m, int state, void* arg, xmlnode x, char* unused1, int unused2);

/**
 * send routing update to peer
 *
 * @param ci the base_connect instance
 * @param host the host to send the routing update for
 * @param is_register true to send a host command, false to send unhost command
 */
static void base_connect_send_routingupdate_peer(conn_info ci, char const* host, bool is_register) {
    xmlnode route_stanza = xmlnode_new_tag_ns("xdb", NULL, NS_SERVER);
    xmlnode_put_attrib_ns(route_stanza, "ns", NULL, NULL, "");
    xmlnode_put_attrib_ns(route_stanza, "from", NULL, NULL, ci->inst->id);
    jid magic_jid = jid_new(xmlnode_pool(route_stanza), is_register ? "host@-internal" : "unhost@-internal");
    jid_set(magic_jid, host, JID_RESOURCE);
    xmlnode_put_attrib_ns(route_stanza, "to", NULL, NULL, jid_full(magic_jid));
    mio_write(ci->io, route_stanza, NULL, 0);
}

/* Deliver packets to the socket io thread */
static result base_connect_deliver(instance i, dpacket p, void* arg) {
    conn_info ci = (conn_info)arg;

    /* Insert the message into the write_queue if we don't have an MIO socket yet.. */
    if (ci->state != conn_AUTHD) {
        conn_write_buf entry = static_cast<conn_write_buf>(pmalloco(p->p, sizeof(_conn_write_buf)));
        entry->packet = p;
        pth_msgport_put(ci->write_queue, (pth_message_t*)entry);
    } else {
	/* Otherwise, write directly to the MIO socket */
        if (ci->dplast == p) {
	    /* don't handle packets that we generated! doh! */
            deliver_fail(p, N_("Circular Reference Detected"));

	    // unregister being responsible for this domain in local router and remote
	    unregister_instance(ci->inst, p->host);
	    base_connect_send_routingupdate_peer(ci, p->host, false);
	} else {
            mio_write(ci->io, p->x, NULL, 0);
	}
    }

    return r_DONE;
}

/* this runs from another thread under mtq */
static void base_connect_connect(void *arg) {
    conn_info ci = (conn_info)arg;
    pth_sleep(2); /* take a break */
    mio_connect(ci->hostip, ci->hostport, base_connect_process_xml, ci, ci->timeout, mio_handlers_new(NULL, NULL, MIO_XML_PARSER));
}

static void base_connect_process_xml(mio m, int state, void* arg, xmlnode x, char* unused1, int unused2) {
    conn_info ci = (conn_info)arg;
    xmlnode cur;
    xmppd::sha1 pwdhash;
    char const* tmp = NULL;

    log_debug2(ZONE, LOGT_XML, "process XML: m:%X state:%d, arg:%X, x:%X", m, state, arg, x);

    switch (state) {
        case MIO_NEW:

            ci->state = conn_OPEN;
            ci->io = m;

            /* Send a stream header to the server */
            log_debug2(ZONE, LOGT_IO, "base_connecting: %X, %X, %s", ci, ci->inst, ci->inst->id); 

            cur = xstream_header(ci->inst->id, NULL);
	    mio_write_root(m, cur, 2);

            return;

        case MIO_XML_ROOT:
            /* Extract stream ID and generate a key to hash */
	    tmp = xmlnode_get_attrib_ns(x, "id", NULL);
	    if (tmp)
		pwdhash.update(tmp);
	    if (ci->secret)
		pwdhash.update(ci->secret);

            /* Build a handshake packet */
            cur = xmlnode_new_tag_ns("handshake", NULL, NS_SERVER);
            xmlnode_insert_cdata(cur, pwdhash.final_hex().c_str(), -1);

            /* Transmit handshake */
	    mio_write(m, cur, NULL, 0);
            xmlnode_free(x);
            return;

        case MIO_XML_NODE:
            /* Only deliver packets after the connection is auth'd */
            if (ci->state == conn_AUTHD) {
                ci->dplast = dpacket_new(x); /* store the addr of the dpacket we're sending to detect circular delevieries */
                deliver(ci->dplast, ci->inst);
                ci->dplast = NULL;
                return;
            }

            /* If a handshake packet is recv'd from the server, we have successfully auth'd -- go ahead and update the connection state */
            if (j_strcmp(xmlnode_get_localname(x), "handshake") == 0 && j_strcmp(xmlnode_get_namespace(x), NS_SERVER) == 0) {
                /* Flush all packets queued up for delivery */
                conn_write_buf b;
                while ((b = (conn_write_buf) pth_msgport_get(ci->write_queue)) != NULL)
                    mio_write(ci->io, b->packet->x, NULL, 0);
                /* Update connection state flag */
                ci->state = conn_AUTHD;

		// if we are configured as uplink, request the routings to the other instances of this process from our peer process
		if (deliver_is_uplink(ci->inst)) {
		    std::set<Glib::ustring> hosts_to_route = deliver_routed_hosts(p_NORM, ci->inst);

		    for (std::set<Glib::ustring>::const_iterator p = hosts_to_route.begin(); p != hosts_to_route.end(); ++p) {
			log_debug2(ZONE, LOGT_DYNAMIC, "base_connect is uplink. Sending routing request: %s", p->c_str());
			xmlnode route_stanza = xmlnode_new_tag_ns("xdb", NULL, NS_SERVER);
			xmlnode_put_attrib_ns(route_stanza, "ns", NULL, NULL, "");
			xmlnode_put_attrib_ns(route_stanza, "from", NULL, NULL, ci->inst->id);
			jid magic_jid = jid_new(xmlnode_pool(route_stanza), "host@-internal");
			jid_set(magic_jid, p->c_str(), JID_RESOURCE);
			xmlnode_put_attrib_ns(route_stanza, "to", NULL, NULL, jid_full(magic_jid));
			mio_write(ci->io, route_stanza, NULL, 0);
		    }
		}
            }
            xmlnode_free(x);
            return;

        case MIO_CLOSED:

            if(ci->state == conn_DIE)
                return; /* server is dying */

            /* Always restart the connection after it closes for any reason */
            ci->state = conn_CLOSED;
            if(ci->tries_left != -1) 
                ci->tries_left--;

            if(ci->tries_left == 0) {
                fprintf(stderr, "Base Connect Failed: service %s was unable to connect to %s:%d, unrecoverable error, exiting", ci->inst->id, ci->hostip, ci->hostport);
                exit(1);
            }

            /* pause 2 seconds, and reconnect */
            log_debug2(ZONE, LOGT_IO, "Base Connect Failed to connect to %s:%d Retry [%d] in 2 seconds...", ci->hostip, ci->hostport, ci->tries_left);
            mtq_send(NULL,ci->mempool,base_connect_connect,(void *)ci);

            return;
    }
}

static void base_connect_kill(void *arg) {
    conn_info ci = (conn_info)arg;
    ci->state = conn_DIE;
}

/**
 * callback that gets notified on routingupdates
 *
 * @param i the instance that (un)registered the routing
 * @param destination the host the gets (un)routed
 * @param is_register true for a registration, false of an unregistration
 * @param arg pointer to the base_connect instance data
 */
static void base_connect_routingupdate(instance i, char const* destination, int is_register, void *arg) {
    conn_info	ci = static_cast<conn_info>(arg);
    // sanity check
    if (!ci || !destination)
	return;

    // we only care for the routingupdates if we are configured to be the uplink
    if (!deliver_is_uplink(ci->inst))
	return;

    // we do not forward default routings
    if (std::string("*") == destination)
	return;

    // do not route updates for hosts we (un)registered ourself
    if (ci->inst == i)
	return;

    // and we have an established connection
    if (!ci->io || ci->state != conn_AUTHD)
	return;

    log_debug2(ZONE, LOGT_DYNAMIC, "base_connect is uplink and has to forward %s", is_register ? "host-command" : "unhost-command");
    base_connect_send_routingupdate_peer(ci, destination, is_register);
}

static result base_connect_config(instance id, xmlnode x, void *arg) {
    char*	secret = NULL;
    int		timeout = 5;
    int		tries = -1;
    char*	ip = NULL;
    int		port = 0;
    conn_info	ci = NULL;
    xht		namespaces = NULL;

    /* Extract info */
    namespaces = xhash_new(3);
    xhash_put(namespaces, "", const_cast<char*>(NS_JABBERD_CONFIGFILE));
    ip = xmlnode_get_data(xmlnode_get_list_item(xmlnode_get_tags(x, "ip", namespaces), 0));
    port = j_atoi(xmlnode_get_data(xmlnode_get_list_item(xmlnode_get_tags(x, "port", namespaces), 0)), 0);
    secret = xmlnode_get_data(xmlnode_get_list_item(xmlnode_get_tags(x, "secret", namespaces), 0));
    timeout = j_atoi(xmlnode_get_data(xmlnode_get_list_item(xmlnode_get_tags(x, "timeout", namespaces), 0)), 5);
    tries = j_atoi(xmlnode_get_data(xmlnode_get_list_item(xmlnode_get_tags(x, "tries", namespaces), 0)), -1);
    xhash_free(namespaces);

    if(id == NULL) {
        log_debug2(ZONE, LOGT_INIT|LOGT_CONFIG, "base_accept_config validating configuration\n");
        if(port == 0 || (secret == NULL)) {
            xmlnode_put_attrib_ns(x, "error", NULL, NULL, "<connect> requires the following subtags: <port>, and <secret>");
            return r_ERR;
        }
        return r_PASS;
    }

    log_debug2(ZONE, LOGT_INIT|LOGT_CONFIG, "Activating configuration: %s\n", xmlnode_serialize_string(x, xmppd::ns_decl_list(), 0));

    /* Allocate a conn structures, using this instances' mempool */
    ci              = static_cast<conn_info>(pmalloco(id->p, sizeof(_conn_info)));
    ci->mempool     = id->p;
    ci->state       = conn_CLOSED;
    ci->inst        = id;
    ci->hostip      = pstrdup(ci->mempool, ip);
    if (ci->hostip == NULL)
	ci->hostip = pstrdup(ci->mempool, "127.0.0.1");
    ci->hostport    = port;
    ci->secret      = pstrdup(ci->mempool, secret);
    ci->write_queue = pth_msgport_create(ci->hostip);
    ci->timeout     = timeout;
    ci->tries_left  = tries;

    /* Register a handler to recieve inbound data for this instance */
    register_phandler(id, o_DELIVER, base_connect_deliver, (void*)ci);

    // Register a handler that gets notified on newly available hosts
    register_routing_update_callback(NULL, base_connect_routingupdate, static_cast<void*>(ci));
     
    /* Make a connection to the host, in another thread */
    mtq_send(NULL,ci->mempool,base_connect_connect,(void *)ci);

    register_shutdown(base_connect_kill, (void *)ci);

    return r_DONE;
}

/**
 * register the connect base handler
 *
 * @param p memory pool used to register the handler for the &lt;connect/&gt; configuration element (must be available for the livetime of jabberd)
 */
void base_connect(pool p) {
    log_debug2(ZONE, LOGT_INIT, "base_connect loading...\n");
    register_config(p, "connect",base_connect_config,NULL);
}
