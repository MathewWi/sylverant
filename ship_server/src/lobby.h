/*
    Sylverant Ship Server
    Copyright (C) 2009, 2010, 2011 Lawrence Sebald

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

#ifndef LOBBY_H
#define LOBBY_H

#include <time.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/queue.h>

#define PACKETS_H_HEADERS_ONLY
#include "packets.h"
#undef PACKETS_H_HEADERS_ONLY

#include "player.h"

#define LOBBY_MAX_CLIENTS 12

/* Forward declaration. */
struct ship_client;
struct block;

#ifndef SHIP_CLIENT_DEFINED
#define SHIP_CLIENT_DEFINED
typedef struct ship_client ship_client_t;
#endif

#ifndef BLOCK_DEFINED
#define BLOCK_DEFINED
typedef struct block block_t;
#endif

#ifndef SHIP_DEFINED
#define SHIP_DEFINED
typedef struct ship ship_t;
#endif

typedef struct lobby_pkt {
    STAILQ_ENTRY(lobby_pkt) qentry;

    ship_client_t *src;
    dc_pkt_hdr_t *pkt;
} lobby_pkt_t;

STAILQ_HEAD(lobby_pkt_queue, lobby_pkt);

struct lobby {
    TAILQ_ENTRY(lobby) qentry;

    pthread_mutex_t mutex;

    uint32_t lobby_id;
    uint32_t type;
    int max_clients;
    int num_clients;

    block_t *block;
    uint32_t flags;

    uint8_t leader_id;
    uint8_t difficulty;
    uint8_t battle;
    uint8_t challenge;

    uint8_t v2;
    uint8_t section;
    uint8_t event;
    uint8_t episode;

    uint8_t max_chal;
    uint8_t legit_check_passed;
    uint8_t legit_check_done;

    int version;
    uint32_t min_level;
    uint32_t max_level;
    uint32_t rand_seed;

    char name[33];
    char passwd[17];
    uint32_t maps[0x20];
    uint16_t highest_item[4];

    ship_client_t *clients[LOBBY_MAX_CLIENTS];

    struct lobby_pkt_queue pkt_queue;
    time_t create_time;

    int (*dropfunc)(struct lobby *l, void *req);
    uint32_t next_item;
};

#ifndef LOBBY_DEFINED
#define LOBBY_DEFINED
typedef struct lobby lobby_t;
#endif

TAILQ_HEAD(lobby_queue, lobby);

/* Possible values for the type parameter. */
#define LOBBY_TYPE_DEFAULT      0x00000001
#define LOBBY_TYPE_GAME         0x00000002
#define LOBBY_TYPE_EP3_GAME     0x00000004

/* Possible values for the flags parameter. */
#define LOBBY_FLAG_BURSTING     0x00000001
#define LOBBY_FLAG_QUESTING     0x00000002
#define LOBBY_FLAG_QUESTSEL     0x00000004
#define LOBBY_FLAG_TEMP_UNAVAIL 0x00000008
#define LOBBY_FLAG_LEGIT_MODE   0x00000010
#define LOBBY_FLAG_LEGIT_CHECK  0x00000020
#define LOBBY_FLAG_DCONLY       0x00000040
#define LOBBY_FLAG_PCONLY       0x00000080
#define LOBBY_FLAG_V1ONLY       0x00000100
#define LOBBY_FLAG_GC_ALLOWED   0x00000200
#define LOBBY_FLAG_SINGLEPLAYER 0x00000400
#define LOBBY_FLAG_EP3          0x00000800

/* Events that can be set on games */
#define GAME_EVENT_NONE         0
#define GAME_EVENT_CHRISTMAS    1
#define GAME_EVENT_21ST         2
#define GAME_EVENT_VALENTINES   3
#define GAME_EVENT_EASTER       4
#define GAME_EVENT_HALLOWEEN    5
#define GAME_EVENT_SONIC        6

/* Events that can be set on lobbies */
#define LOBBY_EVENT_NONE        0
#define LOBBY_EVENT_CHRISTMAS   1
/* 2 is just a normal (no event) lobby... */
#define LOBBY_EVENT_VALENTINES  3
#define LOBBY_EVENT_EASTER      4
#define LOBBY_EVENT_HALLOWEEN   5
#define LOBBY_EVENT_SONIC       6
#define LOBBY_EVENT_NEWYEARS    7
#define LOBBY_EVENT_SPRING      8
#define LOBBY_EVENT_WHITEDAY    9
#define LOBBY_EVENT_WEDDING     10
#define LOBBY_EVENT_AUTUMN      11
#define LOBBY_EVENT_FLAGS       12
#define LOBBY_EVENT_SPRINGFLAG  13
#define LOBBY_EVENT_ALT_NORMAL  14


/* The required level for various difficulties. */
const static int game_required_level[4] = { 1, 20, 40, 80 };

lobby_t *lobby_create_default(block_t *block, uint32_t lobby_id, uint8_t ev);
lobby_t *lobby_create_game(block_t *block, char *name, char *passwd,
                           uint8_t difficulty, uint8_t battle, uint8_t chal,
                           uint8_t v2, int version, uint8_t section,
                           uint8_t event, uint8_t episode, ship_client_t *c);
lobby_t *lobby_create_ep3_game(block_t *block, char *name, char *passwd,
                               uint8_t view_battle, uint8_t section);
void lobby_destroy(lobby_t *l);
void lobby_destroy_noremove(lobby_t *l);

/* Add the client to any available lobby on the current block. */
int lobby_add_to_any(ship_client_t *c);

/* Send a packet to all people in a lobby. */
int lobby_send_pkt_dc(lobby_t *l, ship_client_t *c, void *hdr, int igcheck);
int lobby_send_pkt_bb(lobby_t *l, ship_client_t *c, void *hdr, int igcheck);

/* Send a packet to all Episode 3 clients in a lobby. */
int lobby_send_pkt_ep3(lobby_t *l, ship_client_t *c, void *h);

/* Move the client to the requested lobby, if possible. */
int lobby_change_lobby(ship_client_t *c, lobby_t *req);

/* Remove a player from a lobby without changing their lobby (for instance, if
   they disconnected). */
int lobby_remove_player(ship_client_t *c);

/* Send an information reply packet with information about the lobby. */
int lobby_info_reply(ship_client_t *c, uint32_t lobby);

/* Check if a single player is legit enough for the lobby. */
int lobby_check_player_legit(lobby_t *l, ship_t *s, player_t *pl, uint32_t v);

/* Check if a single client is legit enough for the lobby. */
int lobby_check_client_legit(lobby_t *l, ship_t *s, ship_client_t *c);

/* Finish with a legit check. */
void lobby_legit_check_finish_locked(lobby_t *l);

/* Send out any queued packets when we get a done burst signal. */
int lobby_handle_done_burst(lobby_t *l);

/* Enqueue a packet for later sending (due to a player bursting) */
int lobby_enqueue_pkt(lobby_t *l, ship_client_t *c, dc_pkt_hdr_t *p);

#endif /* !LOBBY_H */
