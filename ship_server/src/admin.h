/*
    Sylverant Ship Server
    Copyright (C) 2011 Lawrence Sebald

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License version 3
    as published by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef ADMIN_H
#define ADMIN_H

#include "clients.h"

/* Some macros for commonly used privilege checks. */
#define LOCAL_GM(c) \
    ((c->privilege & CLIENT_PRIV_LOCAL_GM) && \
     (c->flags & CLIENT_FLAG_LOGGED_IN))

#define GLOBAL_GM(c) \
    ((c->privilege & CLIENT_PRIV_GLOBAL_GM) && \
     (c->flags & CLIENT_FLAG_LOGGED_IN))

#define LOCAL_ROOT(c) \
    ((c->privilege & CLIENT_PRIV_LOCAL_ROOT) && \
     (c->flags & CLIENT_FLAG_LOGGED_IN))

#define GLOBAL_ROOT(c) \
    ((c->privilege & CLIENT_PRIV_GLOBAL_ROOT) && \
     (c->flags & CLIENT_FLAG_LOGGED_IN))

int kill_guildcard(ship_client_t *c, uint32_t gc, const char *reason);

int refresh_quests(ship_client_t *c);
int refresh_gms(ship_client_t *c);
int refresh_limits(ship_client_t *c);

int broadcast_message(ship_client_t *c, const char *message, int prefix);

int schedule_shutdown(ship_client_t *c, uint32_t when, int restart);

int global_ban(ship_client_t *c, uint32_t gc, uint32_t l, const char *reason);

#endif /* !ADMIN_H */
