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
 */
#include "jabberd.h"
#define MAX_INCLUDE_NESTING 20
extern HASHTABLE cmd__line;

xmlnode greymatter__ = NULL;

void do_include(int nesting_level,xmlnode x)
{
    xmlnode cur;
    cur=xmlnode_get_firstchild(x);
    for(;cur!=NULL;)
    {
        if(cur->type!=NTYPE_TAG) 
        {
            cur=xmlnode_get_nextsibling(cur);
            continue;
        }
        if(j_strcmp(xmlnode_get_name(cur),"jabberd:include")==0)
        {
            xmlnode include;
            char *include_file=xmlnode_get_data(cur);
            xmlnode include_x=xmlnode_file(include_file);
            /* check for bad nesting */
            if(nesting_level>MAX_INCLUDE_NESTING)
            {
                printf("ERROR: Included files nested %d levels deep.  Possible Recursion\n",nesting_level);
                exit(1);
            }
            include=cur;
            xmlnode_hide(include);
            /* check to see what to insert...
             * if root tag matches parent tag of the <include/> -- firstchild
             * otherwise, insert the whole file
             */
             if(j_strcmp(xmlnode_get_name(xmlnode_get_parent(cur)),xmlnode_get_name(include_x))==0)
                xmlnode_insert_node(x,xmlnode_get_firstchild(include_x));
             else
                xmlnode_insert_node(x,include_x);
             do_include(nesting_level+1,include_x);
             cur=xmlnode_get_nextsibling(cur);
             continue;
        }
        else 
        {
            do_include(nesting_level,cur);
        }
        cur=xmlnode_get_nextsibling(cur);
    }
}

void cmdline_replace(xmlnode x)
{
    char *flag;
    char *replace_text;
    xmlnode cur=xmlnode_get_firstchild(x);

    for(;cur!=NULL;cur=xmlnode_get_nextsibling(cur))
    {
        if(cur->type!=NTYPE_TAG)continue;
        if(j_strcmp(xmlnode_get_name(cur),"jabberd:cmdline")!=0)
        {
            cmdline_replace(cur);
            continue;
        }
        flag=xmlnode_get_attrib(cur,"flag");
        replace_text=ghash_get(cmd__line,flag);
        if(replace_text==NULL) replace_text=xmlnode_get_data(cur);

        xmlnode_hide(xmlnode_get_firstchild(x));
        xmlnode_insert_cdata(x,replace_text,-1);
        break;
    }
}

int configurate(char *file)
{
    /* XXX-temas:  do this one */
    /* CONFIGXML is the default name for the config file - defined by the build system */
    char def[] = CONFIGXML;
    char *realfile = (char *)def;

    /* if no file name is specified, fall back to the default file */
    if(file != NULL)
        realfile = file;

    /* read and parse file */
    greymatter__ = xmlnode_file(realfile);

    /* was the there a read/parse error? */
    if(greymatter__ == NULL)
    {
        printf("Configuration using %s failed\n",realfile);
        return 1;
    }

    /* check greymatter for additional includes */
    do_include(0,greymatter__);
    cmdline_replace(greymatter__);

    return 0;
}

/* private config handler list */
typedef struct cfg_struct
{
    char *node;
    cfhandler f;
    void *arg;
    struct cfg_struct *next;
} *cfg, _cfg;

cfg cfhandlers__ = NULL;
pool cfhandlers__p = NULL;

/* register a function to handle that node in the config file */
void register_config(char *node, cfhandler f, void *arg)
{
    cfg newg;

    /* if first time */
    if(cfhandlers__p == NULL) cfhandlers__p = pool_new();

    /* create and setup */
    newg = pmalloc_x(cfhandlers__p, sizeof(_cfg), 0);
    newg->node = pstrdup(cfhandlers__p,node);
    newg->f = f;
    newg->arg = arg;

    /* hook into global */
    newg->next = cfhandlers__;
    cfhandlers__ = newg;
}

/* util to scan through registered config callbacks */
cfg cfget(char *node)
{
    cfg next = NULL;

    for(next = cfhandlers__; next != NULL && strcmp(node,next->node) != 0; next = next->next);

    return next;
}

/* execute configuration file */
int configo(int exec)
{
    cfg c;
    xmlnode curx, curx2;
    ptype type;
    instance newi = NULL;
    pool p;

    for(curx = xmlnode_get_firstchild(greymatter__); curx != NULL; curx = xmlnode_get_nextsibling(curx))
    {
        if(xmlnode_get_type(curx) != NTYPE_TAG || strcmp(xmlnode_get_name(curx),"base") == 0)
            continue;

        type = p_NONE;

        if(strcmp(xmlnode_get_name(curx),"log") == 0)
            type = p_LOG;
        if(strcmp(xmlnode_get_name(curx),"xdb") == 0)
            type = p_XDB;
        if(strcmp(xmlnode_get_name(curx),"service") == 0)
            type = p_NORM;

        if(type == p_NONE || xmlnode_get_attrib(curx,"id") == NULL || xmlnode_get_firstchild(curx) == NULL)
        {
            printf("Configuration error in:\n%s\n",xmlnode2str(curx));
            if(type==p_NONE) printf("ERROR: Invalid Tag type: %s\n",xmlnode_get_name(curx));
            if(xmlnode_get_attrib(curx,"id")==NULL)
                printf("ERROR: Section needs an 'id' attribute\n");
            if(xmlnode_get_firstchild(curx)==NULL)
                printf("ERROR: Section Has no data in it\n");
            return 1;
        }

        /* create the instance */
        if(exec)
        {
            p = pool_new();
            newi = pmalloc_x(p, sizeof(_instance), 0);
            newi->id = pstrdup(p,xmlnode_get_attrib(curx,"id"));
            newi->type = type;
            newi->p = p;
            newi->x = curx;
        }

        /* loop through all this sections children */
        for(curx2 = xmlnode_get_firstchild(curx); curx2 != NULL; curx2 = xmlnode_get_nextsibling(curx2))
        {
            /* only handle elements in our namespace */
            if(xmlnode_get_type(curx2) != NTYPE_TAG || xmlnode_get_attrib(curx2, "xmlns") != NULL)
                continue;

            /* run the registered function for this element */
            c = cfget(xmlnode_get_name(curx2));
            if(c == NULL || (c->f)(newi, curx2, c->arg) == r_ERR)
            {
                char *error=pstrdup(xmlnode_pool(curx2),xmlnode_get_attrib(curx2,"error"));
                xmlnode_hide_attrib(curx2,"error");
                printf("Invalid Configuration in instance '%s':\n%s\n",xmlnode_get_attrib(curx,"id"),xmlnode2str(curx2));
                if(c==NULL) printf("ERROR: Unknown Base Tag: %s\n",xmlnode_get_name(curx2));
                else if(error!=NULL)
                {
                    printf("ERROR: Base Handler Returned an Error:\n%s\n",error);
                }
                return 1;
            }
        }
    }

    return 0;
}

