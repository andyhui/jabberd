#include "jabberd.h"

/* ---------------------------------------------------------
   base_connect - Connects to a specified host/port and 
                  exchanges xmlnodes with it over a socket
   ---------------------------------------------------------

   USAGE:
     <connect>
        <ip>1.2.3.4</ip>
	<port>2020</port>
	<secret>foobar</secret>
     </connect>

   TODO: 
   - Add packet aging/delivery heartbeat
   - Handle closing/err'd XML from server connection
*/

/* Connection states */
typedef enum { conn_CLOSED, conn_OPEN, conn_AUTHD } conn_state;

/* conn_info - stores thread data for a connection */
typedef struct
{
     int           socket;	/* Connection socket  */
     conn_state    state;	/* Connection status (closed, opened, auth'd) */
     char*         hostip;	/* Server IP */
     u_short       hostport;	/* Server port */
     char*         secret;	/* Connection secret */
     pool          mempool;	/* Memory pool for this struct */
     pth_msgport_t write_queue;	/* Queue of write_buf packets which need to be written */
     pth_event_t   e_read;	/* Event which is set when socket is ready for reading */
     pth_event_t   e_write;	/* Event which is set when socket is ready for writing */
     pth_event_t   events;	/* Event ring for e_write & e_read */ 
} *conn_info, _conn_info;

/* conn_write_buf - stores a dpacket that needs to be written to the socket */
typedef struct
{
     pth_message_t head;
     dpacket       packet;
} *conn_write_buf, _conn_write_buf;

/* Node which stores a mapping of ip:port to connection structures as vattribs*/
xmlnode base_connect__connlist;

/* base_connect_search_conns - Search connection list for a given ip:port */
conn_info base_connect_search_conns(const char* ip, const char* port)
{
     char searchname[22];

     /* Format ip & port into a search string */
     snprintf(searchname, 21, "%s:%s", ip, port);
     searchname[21] = '\0';

     /* Locate the matching attribute in the connlist node */
     return xmlnode_get_vattrib(base_connect__connlist, (const char*) &searchname);
}

/* base_connect_insert_conn - Associate a connection with a ip:port */
void base_connect_insert_conn(const char* ip, const char* port, conn_info ci)
{
     char searchname[22];

     /* Format ip & port into a search string */
     snprintf(searchname, 21, "%s:%s", ip, port);
     searchname[21] = '\0';

     /* Insert the searchname & conn info as a vattrib */
     xmlnode_put_vattrib(base_connect__connlist, (const char*) &searchname, (void*)ci);
}


/* Deliver packets to the socket io thread */
result base_connect_deliver(instance i, dpacket p, void* arg)
{
     conn_info ci      = (conn_info)arg;
     conn_write_buf wb = NULL;

     /* Allocate a new write buffer */
     wb = pmalloco(p->p, sizeof(_conn_write_buf));
     wb->packet = p;

     /* Put the buffer in the io thread's message port */
     pth_msgport_put(ci->write_queue, (pth_message_t*)wb);

     return r_OK;
}

void base_connect_handle_xstream_event(int type, xmlnode x, void* arg)
{
     conn_info ci  = (conn_info)arg;
     xmlnode   cur = NULL;
     char*     strbuf = NULL;
     char      hashbuf[40];     

     switch(type)
     {
     case XSTREAM_ROOT:
	  /* Extract the stream ID and generate a key to hash*/
	  strbuf = spools(x->p, ci->secret, xmlnode_get_attrib(x, "id"), x->p);
	  /* Calculate SHA hash */
	  shahash_r(strbuf, hashbuf);
	  /* Build a handshake packet */
	  cur = xmlnode_new_tag_pool(x->p, "handshake");
	  xmlnode_insert_cdata(cur, (const char*)&hashbuf, sizeof(hashbuf));
	  /* Transmit handshake request */	  
	  strbuf = xmlnode2str(cur);
	  pth_write(ci->socket, strbuf, strlen(strbuf));
	  break;
     case XSTREAM_NODE:
	  /* Only deliver packets after the connection is auth'd */
	  if (ci->state == conn_AUTHD)
	  {
	       deliver(dpacket_new(x), NULL);
	  }
	  else
	  {
	       /* If a handshake packet is recv'd from the server, we
		  have successfully auth'd */
	       if (strcmp(xmlnode_get_name(x), "handshake") == 0)
		    ci->state = conn_AUTHD;
	       /* Drop the packet, regardless */
	       pool_free(x->p);
	  }
	  break;
     case XSTREAM_CLOSE:
     case XSTREAM_ERR:
	  /* FIXME: Who knows? The _SHADOW_ knows. */
     }
}

/* IO thread for a socket */
void* base_connect_process_io(void* arg)
{
     conn_info ci = (conn_info)arg;
     /* XML processor */
     xstream xs;

     /* Read vars */
     char readbuf[1024];
     int  readlen = 0;

     /* Write vars */
     conn_write_buf cwb;
     char*          writebuf = NULL;

     /* Allocate an xstream for this socket */
     xs = xstream_new(ci->mempool, base_connect_handle_xstream_event, arg);

     /* Attempt to connect... */
     while (ci->socket < 0)
     {
	  /* Log the attempt to connect */
	  log_debug(ZONE, "Attempting to connect to: %s : %d\n", ci->hostip, ci->hostport);
	  /* Attempt to connect */
	  ci->socket = make_netsocket(ci->hostport, ci->hostip, NETSOCKET_CLIENT);
	  /* Sleep for a bit... */
	  if (ci->socket < 0) 
	       pth_nap(pth_time(5, 0));
     }

    /* Setup initial event ring for this socket */
     ci->e_read = pth_event(PTH_EVENT_FD|PTH_UNTIL_FD_READABLE, ci->socket);
     ci->events = pth_event_concat(ci->e_read, NULL);

     /* Update state flag */
     ci->state = conn_OPEN;

     /* Transmit stream header */  
     pth_write(ci->socket, "<root>", strlen("<root>"));

     /* Loop on events */
     while (pth_wait(ci->events) > 0)
     {
	  /* Data is available for reading */
	  if (pth_event_occurred(ci->e_read))
	  {
	       readlen = pth_read(ci->socket, readbuf, sizeof(readbuf));
	       if (readlen <= 0)
	       {
		    log_debug(ZONE, "Socket read error: %d\n", ci->socket);
		    break;
	       }
	       if (xstream_eat(xs, readbuf, readlen) > XSTREAM_NODE)
		    break;
	  }
	  /* Data is available to be written */
	  if (pth_event_occurred(ci->e_write))
	  {
	       /* Get the packet */
	       cwb = (conn_write_buf)pth_msgport_get(ci->write_queue);
	       
	       /* Serialize the packet's xmlnode.. */
	       writebuf = xmlnode2str(cwb->packet->x);

	       /* Write the raw buffer */
	       if (pth_write(ci->socket, writebuf, strlen(writebuf)) < 0)
	       {
		    log_debug(ZONE, "Socket write error: %d\n", ci->socket);
		    break;
	       }

	       /* Data is sent, release the packet */
	       pool_free(cwb->packet->p);
	  }
     }

     /* If an error has occurred on the socket, we need to cleanup the socket
	and reconnect -- messages in the queue don't need to be bounced */

     /* Cleanup.. */
     close(ci->socket);
     ci->state  = conn_CLOSED;
     ci->socket = -1;
     pth_event_free(ci->e_read, PTH_FREE_THIS);
     pth_event_free(ci->e_write, PTH_FREE_THIS);

     /* Start a new thread to handle IO on the socket */
     pth_spawn(PTH_ATTR_DEFAULT, base_connect_process_io, (void*)ci);

     return NULL;

}

result base_connect_config(instance id, xmlnode x, void *arg)
{
     char*     ip     = NULL;
     char*     port   = NULL;
     char*     secret = NULL;
     conn_info ci = NULL;

     if(id == NULL)
     {
	  log_debug(ZONE, "Validating configuration..\n");
	  if(xmlnode_get_data(xmlnode_get_tag(x, "ip")) == NULL ||
	     xmlnode_get_data(xmlnode_get_tag(x, "port")) == NULL || 
	     xmlnode_get_data(xmlnode_get_tag(x, "secret")) == NULL)
	       return r_ERR;
	  return r_PASS;
    }
     
     /* Extract info */
     ip     = xmlnode_get_data(xmlnode_get_tag(x, "ip"));
     port   = xmlnode_get_data(xmlnode_get_tag(x, "port"));
     secret = xmlnode_get_data(xmlnode_get_tag(x, "secret"));

     /* Search for an existing connection structure */
     ci = base_connect_search_conns(ip, port);

     /* No connection was found, so create one.. */
     if (ci == NULL)
     {
	  /* Allocate a conn structures, using this instances' mempool */
	  ci              = pmalloco(id->p, sizeof(_conn_info));
	  ci->mempool     = id->p;
	  ci->state       = conn_CLOSED;
	  ci->socket      = -1;
	  ci->hostip      = pstrdup(ci->mempool, ip);
	  ci->hostport    = atoi(port);
	  ci->secret      = pstrdup(ci->mempool, secret);
	  ci->write_queue = pth_msgport_create(ci->hostip);
	  /* Insert the structure into the connection list */
	  base_connect_insert_conn(ip, port, ci);

	  /* Start a new thread to handle IO on the socket */
	  pth_spawn(PTH_ATTR_DEFAULT, base_connect_process_io, (void*)ci);
     }

     /* Register a handler to recieve inbound data for this instance */
     register_phandler(id, o_DELIVER, base_connect_deliver, (void*)ci);
     
     log_debug(ZONE, "Activating configuration: %s\n", xmlnode2str(x));
     return r_OK;
}

void base_connect(void)
{
    printf("base_connect loading...\n");

    register_config("connect",base_connect_config,NULL);
}
