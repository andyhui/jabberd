/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 *  Jabber
 *  Copyright (C) 1998-1999 The Jabber Team http://jabber.org/
 *
 *  services.c -- services API
 *
 */

#include "jsm.h"


void js_authreg(jsmi si, jpacket p, HASHTABLE ht)
{
    udata user;
    char *u, *ul, *from, *to;
    jid id;

    /* setup session trackers */
    from = xmlnode_get_attrib(p->x,"sfrom");
    to = xmlnode_get_attrib(p->x,"sto");

    /* get the username supplied by the client */
    u = xmlnode_get_tag_data(p->iq,"username");
    if(u != NULL)
    {
        /* enforce the username to lowercase for registration */
        for(ul = u;*ul != '\0'; ul++)
            *ul = tolower(*ul);

        /* see if the username is valid */
        id = jid_new(p->p,xmlnode_get_attrib(p->x,"sto"));
        jid_set(id,u,JID_USER);
    }

    /* was the username acceptable */
    if(id == NULL || id->user == NULL)
    {
            jutil_error(p->x, TERROR_NOTACCEPTABLE);

    }else if(NSCHECK(p->iq,NS_AUTH)){ /* is this an auth request? */

        log_debug(ZONE,"auth request");

        /* attempt to fetch user data based on the username */
        user = js_user(si, id, ht);
        jid_set(id,xmlnode_get_tag_data(p->iq,"resource"),JID_RESOURCE);
        if(user == NULL || id->resource == NULL)
            jutil_error(p->x, TERROR_AUTH);
        else if(!js_mapi_call(si, e_AUTH, p, user, NULL))
            jutil_error(p->x, TERROR_INTERNAL);

        /* create session if the auth was ok'd */
        jpacket_reset(p);
        if(jpacket_subtype(p) == JPACKET__RESULT)
        {
            js_session_new(si, id, jid_new(p->p,from));
            to = jid_full(id);
        }

    }else if(NSCHECK(p->iq,NS_REGISTER)){ /* is this a registration request? */

        log_debug(ZONE,"registration request");

        /* try to register via a module */
        if(!js_mapi_call(si, e_REGISTER, p, NULL, NULL))
            jutil_error(p->x, TERROR_NOTIMPL);

    }else{ /* unknown namespace */

        jutil_error(p->x, TERROR_AUTH);
    }

    /* make sure packet goes back to the other side of the session */
    xmlnode_put_attrib(p->x,"sfrom",to);
    xmlnode_put_attrib(p->x,"sto",from);

    deliver(dpacket_new(p->x), si->i);
}

