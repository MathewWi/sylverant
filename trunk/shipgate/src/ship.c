/*
    Sylverant Shipgate
    Copyright (C) 2009, 2010 Lawrence Sebald

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <sys/socket.h>

#include <openssl/rc4.h>

#include <sylverant/debug.h>
#include <sylverant/database.h>
#include <sylverant/sha4.h>
#include <sylverant/mtwist.h>
#include <sylverant/md5.h>

#include "ship.h"
#include "shipgate.h"

#define CLIENT_PRIV_LOCAL_GM    0x00000001
#define CLIENT_PRIV_GLOBAL_GM   0x00000002
#define CLIENT_PRIV_LOCAL_ROOT  0x00000004
#define CLIENT_PRIV_GLOBAL_ROOT 0x00000008

/* Database connection */
extern sylverant_dbconn_t conn;

static uint8_t recvbuf[65536];

/* Create a new connection, storing it in the list of ships. */
ship_t *create_connection(int sock, in_addr_t addr) {
    ship_t *rv;
    uint32_t i;

    rv = (ship_t *)malloc(sizeof(ship_t));

    if(!rv) {
        perror("malloc");
        return NULL;
    }

    memset(rv, 0, sizeof(ship_t));

    /* Store basic parameters in the client structure. */
    rv->sock = sock;
    rv->conn_addr = addr;
    rv->last_message = time(NULL);

    for(i = 0; i < 4; ++i) {
        rv->ship_nonce[i] = (uint8_t)genrand_int32();
        rv->gate_nonce[i] = (uint8_t)genrand_int32();
    }

    /* Send the client the welcome packet, or die trying. */
    if(send_welcome(rv)) {
        close(sock);
        free(rv);
        return NULL;
    }

    /* Insert it at the end of our list, and we're done. */
    TAILQ_INSERT_TAIL(&ships, rv, qentry);
    return rv;
}

/* Destroy a connection, closing the socket and removing it from the list. */
void destroy_connection(ship_t *c) {
    char query[256];
    ship_t *i;

    debug(DBG_LOG, "Closing connection with %s\n", c->name);

    TAILQ_REMOVE(&ships, c, qentry);

    if(c->key_idx) {
        /* Send a status packet to everyone telling them its gone away */
        TAILQ_FOREACH(i, &ships, qentry) {
            send_ship_status(i, c, 0);
        }

        /* Remove the ship from the online_ships table. */
        sprintf(query, "DELETE FROM online_ships WHERE ship_id='%hu'",
                c->key_idx);

        if(sylverant_db_query(&conn, query)) {
            debug(DBG_ERROR, "Couldn't clear %s from the online_ships table\n",
                  c->name);
        }
    }

    /* Clean up the ship's structure. */
    if(c->sock >= 0) {
        close(c->sock);
    }

    if(c->recvbuf) {
        free(c->recvbuf);
    }

    if(c->sendbuf) {
        free(c->sendbuf);
    }

    free(c);
}

/* Handle a ship's login response. */
static int handle_shipgate_login(ship_t *c, shipgate_login_reply_pkt *pkt) {
    char query[256];
    ship_t *j;
    uint8_t key[128], hash[64];
    int k = ntohs(pkt->ship_key), i;
    void *result;
    char **row;
    uint32_t pver = c->proto_ver = ntohl(pkt->proto_ver);
    uint16_t menu_code = ntohs(pkt->menu_code);

    /* Check the protocol version for support */
    if(pver < SHIPGATE_MINIMUM_PROTO_VER || pver > SHIPGATE_MAXIMUM_PROTO_VER) {
        debug(DBG_WARN, "Invalid protocol version: %lu\n", pver);

        send_error(c, SHDR_TYPE_LOGIN, SHDR_RESPONSE | SHDR_FAILURE,
                   ERR_LOGIN_BAD_PROTO, NULL, 0);
        return -1;
    }

    /* Attempt to grab the key for this ship. */
    sprintf(query, "SELECT rc4key, main_menu FROM ship_data WHERE idx='%u'", k);

    if(sylverant_db_query(&conn, query)) {
        debug(DBG_WARN, "Couldn't query the database\n");
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));
        send_error(c, SHDR_TYPE_LOGIN, SHDR_RESPONSE | SHDR_FAILURE,
                   ERR_BAD_ERROR, NULL, 0);
        return -1;
    }

    if((result = sylverant_db_result_store(&conn)) == NULL ||
       (row = sylverant_db_result_fetch(result)) == NULL) {
        debug(DBG_WARN, "Invalid index %d\n", k);
        send_error(c, SHDR_TYPE_LOGIN, SHDR_RESPONSE | SHDR_FAILURE,
                   ERR_LOGIN_BAD_KEY, NULL, 0);
        return -1;
    }

    /* Check the menu code for validity */
    if(menu_code && (!isalpha(menu_code & 0xFF) | !isalpha(menu_code >> 8))) {
        debug(DBG_WARN, "Bad menu code for id: %d\n", k);
        send_error(c, SHDR_TYPE_LOGIN, SHDR_RESPONSE | SHDR_FAILURE,
                   ERR_LOGIN_BAD_MENU, NULL, 0);
        return -1;
    }

    /* If the ship requests the main menu and they aren't allowed there, bail */
    if(!menu_code && !atoi(row[1])) {
        debug(DBG_WARN, "Invalid menu code for id: %d\n", k);
        send_error(c, SHDR_TYPE_LOGIN, SHDR_RESPONSE | SHDR_FAILURE,
                   ERR_LOGIN_INVAL_MENU, NULL, 0);
        return -1;
    }

    /* Grab the key from the result */
    memcpy(key, row[0], 128);
    sylverant_db_result_free(result);

    /* Apply the nonces */
    for(i = 0; i < 128; i += 4) {
        key[i + 0] ^= c->gate_nonce[0];
        key[i + 1] ^= c->gate_nonce[1];
        key[i + 2] ^= c->gate_nonce[2];
        key[i + 3] ^= c->gate_nonce[3];
    }

    /* Hash the key with SHA-512, and use that as our final key. */
    sha4(key, 128, hash, 0);
    RC4_set_key(&c->gate_key, 64, hash);

    /* Calculate the final ship key. */
    for(i = 0; i < 128; i += 4) {
        key[i + 0] ^= c->ship_nonce[0];
        key[i + 1] ^= c->ship_nonce[1];
        key[i + 2] ^= c->ship_nonce[2];
        key[i + 3] ^= c->ship_nonce[3];
    }

    /* Hash the key with SHA-512, and use that as our final key. */
    sha4(key, 128, hash, 0);
    RC4_set_key(&c->ship_key, 64, hash);

    c->remote_addr = pkt->ship_addr;
    c->local_addr = pkt->int_addr;
    c->port = ntohs(pkt->ship_port);
    c->key_idx = k;
    c->clients = ntohs(pkt->clients);
    c->games = ntohs(pkt->games);
    c->flags = ntohl(pkt->flags);
    c->menu_code = menu_code;
    strcpy(c->name, pkt->name);

    sprintf(query, "INSERT INTO online_ships(name, players, ip, port, int_ip, "
            "ship_id, gm_only, games, menu_code) VALUES ('%s', '%hu', '%u', "
            "'%hu', '%u', '%u', '%d', '%hu', '%hu')", c->name, c->clients,
            ntohl(c->remote_addr), c->port, ntohl(c->local_addr), c->key_idx,
            !!(c->flags & LOGIN_FLAG_GMONLY), c->games, c->menu_code);

    if(sylverant_db_query(&conn, query)) {
        debug(DBG_WARN, "Couldn't add %s to the online_ships table.\n",
              c->name);
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));
        c->key_set = 0;
        send_error(c, SHDR_TYPE_LOGIN, SHDR_RESPONSE | SHDR_FAILURE,
                   ERR_BAD_ERROR, NULL, 0);
        return -1;
    }

    /* Send a status packet to each of the ships. */
    TAILQ_FOREACH(j, &ships, qentry) {
        send_ship_status(j, c, 1);

        /* Send this ship to the new ship, as long as that wasn't done just
           above here. */
        if(j != c) {
            send_ship_status(c, j, 1);
        }
    }

    /* Hooray for misusing functions! */
    if(send_error(c, SHDR_TYPE_LOGIN, SHDR_RESPONSE, ERR_NO_ERROR, NULL, 0)) {
        return -1;
    }
    else {
        c->key_set = 1;
        return 0;
    }
}

/* Handle a ship's update counters packet. */
static int handle_count(ship_t *c, shipgate_cnt_pkt *pkt) {
    char query[256];
    ship_t *j;

    c->clients = ntohs(pkt->clients);
    c->games = ntohs(pkt->games);

    sprintf(query, "UPDATE online_ships SET players='%hu', games='%hu' WHERE "
            "ship_id='%u'", c->clients, c->games, c->key_idx);
    if(sylverant_db_query(&conn, query)) {
        debug(DBG_WARN, "Couldn't update ship %s player/game count", c->name);
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));
    }

    /* Update all of the ships */
    TAILQ_FOREACH(j, &ships, qentry) {
        send_counts(j, c->key_idx, c->clients, c->games);
    }

    return 0;
}

/* Handle a ship's forwarded Dreamcast packet. */
static int handle_dreamcast(ship_t *c, shipgate_fw_pkt *pkt) {
    uint8_t type = pkt->pkt.pkt_type;
    ship_t *i;
    uint32_t tmp;

    debug(DBG_LOG, "DC: Received %02X\n", type);

    switch(type) {
        case SHIP_GUILD_SEARCH_TYPE:
        case SHIP_SIMPLE_MAIL_TYPE:
            /* Forward these to all ships other than the sender. */
            TAILQ_FOREACH(i, &ships, qentry) {
                if(i != c && !(i->flags & LOGIN_FLAG_PROXY)) {
                    forward_dreamcast(i, &pkt->pkt, c->key_idx);
                }
            }

            return 0;

        case SHIP_DC_GUILD_REPLY_TYPE:
            /* Send this one to the original sender. */
            tmp = ntohl(pkt->ship_id);

            TAILQ_FOREACH(i, &ships, qentry) {
                if(i->key_idx == tmp) {
                    return forward_dreamcast(i, &pkt->pkt, c->key_idx);
                }
            }

            return 0;

        default:
            /* Warn the ship that sent the packet, then drop it */
            send_error(c, SHDR_TYPE_DC, SHDR_FAILURE, ERR_GAME_UNK_PACKET,
                       (uint8_t *)pkt, ntohs(pkt->hdr.pkt_len));
            return 0;
    }
}

/* Handle a ship's forwarded PC packet. */
static int handle_pc(ship_t *c, shipgate_fw_pkt *pkt) {
    uint8_t type = pkt->pkt.pkt_type;
    ship_t *i;

    debug(DBG_LOG, "PC: Received %02X\n", type);

    switch(type) {
        case SHIP_SIMPLE_MAIL_TYPE:
            /* Forward these to all ships other than the sender. */
            TAILQ_FOREACH(i, &ships, qentry) {
                if(i != c && !(i->flags & LOGIN_FLAG_PROXY)) {
                    forward_pc(i, &pkt->pkt, c->key_idx);
                }
            }

            return 0;

        default:
            /* Warn the ship that sent the packet, then drop it */
            send_error(c, SHDR_TYPE_PC, SHDR_FAILURE, ERR_GAME_UNK_PACKET,
                       (uint8_t *)pkt, ntohs(pkt->hdr.pkt_len));
            return 0;
    }
}

/* Handle a ship's save character data packet. */
static int handle_cdata(ship_t *c, shipgate_char_data_pkt *pkt) {
    uint32_t gc, slot;
    char query[4096];

    gc = ntohl(pkt->guildcard);
    slot = ntohl(pkt->slot);

    /* Delete any character data already exising in that slot. */
    sprintf(query, "DELETE FROM character_data WHERE guildcard='%u' AND "
            "slot='%u'", gc, slot);

    if(sylverant_db_query(&conn, query)) {
        debug(DBG_WARN, "Couldn't remove old character data (%u: %u)\n",
              gc, slot);
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));

        send_error(c, SHDR_TYPE_CDATA, SHDR_RESPONSE | SHDR_FAILURE,
                   ERR_BAD_ERROR, (uint8_t *)&pkt->guildcard, 8);
        return 0;
    }

    /* Build up the store query for it. */
    sprintf(query, "INSERT INTO character_data(guildcard, slot, data) VALUES "
            "('%u', '%u', '", gc, slot);
    sylverant_db_escape_str(&conn, query + strlen(query), (char *)pkt->data,
                            1052);
    strcat(query, "')");

    if(sylverant_db_query(&conn, query)) {
        debug(DBG_WARN, "Couldn't save character data (%u: %u)\n", gc, slot);
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));

        send_error(c, SHDR_TYPE_CDATA, SHDR_RESPONSE | SHDR_FAILURE,
                   ERR_BAD_ERROR, (uint8_t *)&pkt->guildcard, 8);
        return 0;
    }

    /* Return success (yeah, bad use of this function, but whatever). */
    return send_error(c, SHDR_TYPE_CDATA, SHDR_RESPONSE, ERR_NO_ERROR,
                      (uint8_t *)&pkt->guildcard, 8);
}

/* Handle a ship's character data request packet. */
static int handle_creq(ship_t *c, shipgate_char_req_pkt *pkt) {
    uint32_t gc, slot;
    char query[256];
    uint8_t data[1052];
    void *result;
    char **row;

    gc = ntohl(pkt->guildcard);
    slot = ntohl(pkt->slot);

    /* Build the query asking for the data. */
    sprintf(query, "SELECT data FROM character_data WHERE guildcard='%u' AND "
            "slot='%u'", gc, slot);

    if(sylverant_db_query(&conn, query)) {
        debug(DBG_WARN, "Couldn't fetch character data (%u: %u)\n", gc, slot);
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));

        send_error(c, SHDR_TYPE_CREQ, SHDR_RESPONSE | SHDR_FAILURE,
                   ERR_BAD_ERROR, (uint8_t *)&pkt->guildcard, 8);
        return 0;
    }

    /* Grab the data we got. */
    if((result = sylverant_db_result_store(&conn)) == NULL) {
        debug(DBG_WARN, "Couldn't fetch character data (%u: %u)\n", gc, slot);
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));

        send_error(c, SHDR_TYPE_CREQ, SHDR_RESPONSE | SHDR_FAILURE,
                   ERR_BAD_ERROR, (uint8_t *)&pkt->guildcard, 8);
        return 0;
    }

    if((row = sylverant_db_result_fetch(result)) == NULL) {
        sylverant_db_result_free(result);
        debug(DBG_WARN, "No saved character data (%u: %u)\n", gc, slot);
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));

        send_error(c, SHDR_TYPE_CREQ, SHDR_RESPONSE | SHDR_FAILURE,
                   ERR_CREQ_NO_DATA, (uint8_t *)&pkt->guildcard, 8);
        return 0;
    }

    /* Grab the data from the result */
    memcpy(data, row[0], 1052);
    sylverant_db_result_free(result);

    /* Send the data back to the ship. */
    return send_cdata(c, gc, slot, data);
}

/* Handle a GM login request coming from a ship. */
static int handle_gmlogin(ship_t *c, shipgate_gmlogin_req_pkt *pkt) {
    uint32_t gc, block;
    char query[256];
    void *result;
    char **row;
    int account_id;
    int i;
    unsigned char hash[16];
    uint8_t priv;

    gc = ntohl(pkt->guildcard);
    block = ntohl(pkt->block);

    /* Build the query asking for the data. */
    sprintf(query, "SELECT account_id FROM guildcards WHERE guildcard='%u'",
            gc);

    if(sylverant_db_query(&conn, query)) {
        debug(DBG_WARN, "Couldn't fetch account id (%u)\n", gc);
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));

        return send_error(c, SHDR_TYPE_GMLOGIN, SHDR_FAILURE, ERR_BAD_ERROR,
                          (uint8_t *)&pkt->guildcard, 8);
    }

    /* Grab the data we got. */
    if((result = sylverant_db_result_store(&conn)) == NULL) {
        debug(DBG_WARN, "Couldn't fetch account id (%u)\n", gc);
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));

        return send_error(c, SHDR_TYPE_GMLOGIN, SHDR_FAILURE, ERR_BAD_ERROR,
                          (uint8_t *)&pkt->guildcard, 8);
    }

    if((row = sylverant_db_result_fetch(result)) == NULL) {
        sylverant_db_result_free(result);
        debug(DBG_WARN, "No account data (%u)\n", gc);

        return send_error(c, SHDR_TYPE_GMLOGIN, SHDR_FAILURE,
                          ERR_GMLOGIN_NO_ACC, (uint8_t *)&pkt->guildcard, 8);
    }

    /* Grab the data from the result */
    account_id = atoi(row[0]);
    sylverant_db_result_free(result);

    /* Now, attempt to fetch the gm status of the account. */
    sprintf(query, "SELECT password, regtime, privlevel FROM account_data WHERE"
            " account_id='%d' AND username='%s' AND privlevel>'0'", account_id,
            pkt->username);

    if(sylverant_db_query(&conn, query)) {
        debug(DBG_WARN, "Couldn't lookup account data (%d)\n", account_id);
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));

        return send_error(c, SHDR_TYPE_GMLOGIN, SHDR_FAILURE, ERR_BAD_ERROR,
                          (uint8_t *)&pkt->guildcard, 8);
    }

    /* Grab the data we got. */
    if((result = sylverant_db_result_store(&conn)) == NULL) {
        debug(DBG_WARN, "Couldn't fetch account data (%d)\n", account_id);
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));

        return send_error(c, SHDR_TYPE_GMLOGIN, SHDR_FAILURE, ERR_BAD_ERROR,
                          (uint8_t *)&pkt->guildcard, 8);
    }

    if((row = sylverant_db_result_fetch(result)) == NULL) {
        sylverant_db_result_free(result);
        debug(DBG_LOG, "Failed GM login - not gm (%s: %d)\n", pkt->username,
              account_id);

        return send_error(c, SHDR_TYPE_GMLOGIN, SHDR_FAILURE,
                          ERR_GMLOGIN_NOT_GM, (uint8_t *)&pkt->guildcard, 8);
    }

    /* Check the password. */
    sprintf(query, "%s_%s_salt", pkt->password, row[1]);
    md5((unsigned char *)query, strlen(query), hash);

    query[0] = '\0';
    for(i = 0; i < 16; ++i) {
        sprintf(query, "%s%02x", query, hash[i]);
    }

    for(i = 0; i < strlen(row[0]); ++i) {
        row[0][i] = tolower(row[0][i]);
    }

    if(strcmp(row[0], query)) {
        debug(DBG_LOG, "Failed GM login - bad password (%d)\n", account_id);
        sylverant_db_result_free(result);

        return send_error(c, SHDR_TYPE_GMLOGIN, SHDR_FAILURE, ERR_BAD_ERROR,
                          (uint8_t *)&pkt->guildcard, 8);
    }

    /* Grab the privilege level out of the packet */
    priv = (uint8_t)atoi(row[2]);

    /* Filter out any privileges that don't make sense. Can't have global GM
       without local GM support. Also, anyone set as a root this way must have
       BOTH root bits set, not just one! */
    if(((priv & CLIENT_PRIV_GLOBAL_GM) && !(priv & CLIENT_PRIV_LOCAL_GM)) ||
       ((priv & CLIENT_PRIV_GLOBAL_ROOT) && !(priv & CLIENT_PRIV_LOCAL_ROOT)) ||
       ((priv & CLIENT_PRIV_LOCAL_ROOT) && !(priv & CLIENT_PRIV_GLOBAL_ROOT))) {
        debug(DBG_WARN, "Invalid privileges on account %d: %02x\n", account_id,
              priv);
        sylverant_db_result_free(result);

        return send_error(c, SHDR_TYPE_GMLOGIN, SHDR_FAILURE, ERR_BAD_ERROR,
                          (uint8_t *)&pkt->guildcard, 8);
    }

    /* We're done if we got this far. */
    sylverant_db_result_free(result);

    /* Send a success message. */
    return send_gmreply(c, gc, block, 1, priv);
}

/* Handle a ban request coming from a ship. */
static int handle_ban(ship_t *c, shipgate_ban_req_pkt *pkt, uint16_t type) {
    uint32_t req, target, until;
    char query[1024];
    void *result;
    char **row;
    int account_id;

    req = ntohl(pkt->req_gc);
    target = ntohl(pkt->target);
    until = ntohl(pkt->until);

    /* Make sure the requester has permission. */
    sprintf(query, "SELECT account_id FROM guildcards NATURAL JOIN "
            "account_data  WHERE guildcard='%u' AND privlevel>'2'", req);

    if(sylverant_db_query(&conn, query)) {
        debug(DBG_WARN, "Couldn't fetch account data (%u)\n", req);
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));

        return send_error(c, type, SHDR_FAILURE, ERR_BAD_ERROR, 
                          (uint8_t *)&pkt->req_gc, 16);
    }

    /* Grab the data we got. */
    if((result = sylverant_db_result_store(&conn)) == NULL) {
        debug(DBG_WARN, "Couldn't fetch account data (%u)\n", req);
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));

        return send_error(c, type, SHDR_FAILURE, ERR_BAD_ERROR,
                          (uint8_t *)&pkt->req_gc, 16);
    }

    if((row = sylverant_db_result_fetch(result)) == NULL) {
        sylverant_db_result_free(result);
        debug(DBG_WARN, "No account data or not gm (%u)\n", req);

        return send_error(c, type, SHDR_FAILURE, ERR_BAN_NOT_GM,
                          (uint8_t *)&pkt->req_gc, 16);
    }

    /* We've verified they're legit, continue on. */
    account_id = atoi(row[0]);
    sylverant_db_result_free(result);

    /* Build up the ban insert query. */
    sprintf(query, "INSERT INTO bans(enddate, setby, reason) VALUES "
            "('%u', '%u', '", until, account_id);
    sylverant_db_escape_str(&conn, query + strlen(query), (char *)pkt->message,
                            strlen(pkt->message));
    strcat(query, "')");

    if(sylverant_db_query(&conn, query)) {
        debug(DBG_WARN, "Could not insert ban into database\n");
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));

        return send_error(c, type, SHDR_FAILURE, ERR_BAD_ERROR,
                          (uint8_t *)&pkt->req_gc, 16);
    }

    /* Now that we have that, add the ban to the right table... */
    switch(type) {
        case SHDR_TYPE_GCBAN:
            sprintf(query, "INSERT INTO guildcard_bans(ban_id, guildcard) "
                    "VALUES(LAST_INSERT_ID(), '%u')", ntohl(pkt->target));
            break;

        case SHDR_TYPE_IPBAN:
            sprintf(query, "INSERT INTO ip_bans(ban_id, addr) VALUES("
                    "LAST_INSERT_ID(), '%u')", ntohl(pkt->target));
            break;

        default:
            return send_error(c, type, SHDR_FAILURE, ERR_BAN_BAD_TYPE,
                              (uint8_t *)&pkt->req_gc, 16);
    }

    if(sylverant_db_query(&conn, query)) {
        debug(DBG_WARN, "Could not insert ban into database (part 2)\n");
        debug(DBG_WARN, "%s\n", sylverant_db_error(&conn));

        return send_error(c, type, SHDR_FAILURE, ERR_BAD_ERROR, 
                          (uint8_t *)&pkt->req_gc, 16);
    }

    /* Another misuse of the error function, but whatever */
    return send_error(c, type, SHDR_RESPONSE, ERR_NO_ERROR,
                      (uint8_t *)&pkt->req_gc, 16);
}

/* Process one ship packet. */
int process_ship_pkt(ship_t *c, shipgate_hdr_t *pkt) {
    uint16_t type = ntohs(pkt->pkt_type);
    uint16_t flags = ntohs(pkt->flags);

    debug(DBG_LOG, "Receieved type 0x%04X\n", type);

    switch(type) {
        case SHDR_TYPE_LOGIN:
            if(!(flags & SHDR_RESPONSE)) {
                debug(DBG_WARN, "Client sent invalid login response\n");
                return -1;
            }

            return handle_shipgate_login(c, (shipgate_login_reply_pkt *)pkt);

        case SHDR_TYPE_COUNT:
            return handle_count(c, (shipgate_cnt_pkt *)pkt);

        case SHDR_TYPE_DC:
            return handle_dreamcast(c, (shipgate_fw_pkt *)pkt);

        case SHDR_TYPE_PC:
            return handle_pc(c, (shipgate_fw_pkt *)pkt);

        case SHDR_TYPE_PING:
            /* If this is a ping request, reply. Otherwise, ignore it, the work
               has already been done. */
            if(!(flags & SHDR_RESPONSE)) {
                return send_ping(c, 1);
            }

            return 0;

        case SHDR_TYPE_CDATA:
            return handle_cdata(c, (shipgate_char_data_pkt *)pkt);

        case SHDR_TYPE_CREQ:
            return handle_creq(c, (shipgate_char_req_pkt *)pkt);

        case SHDR_TYPE_GMLOGIN:
            return handle_gmlogin(c, (shipgate_gmlogin_req_pkt *)pkt);

        case SHDR_TYPE_GCBAN:
        case SHDR_TYPE_IPBAN:
            return handle_ban(c, (shipgate_ban_req_pkt *)pkt, type);

        default:
            return -3;
    }
}

/* Handle incoming data to the shipgate. */
int handle_pkt(ship_t *c) {
    ssize_t sz;
    uint16_t pkt_sz;
    int rv = 0;
    unsigned char *rbp;
    void *tmp;

    /* If we've got anything buffered, copy it out to the main buffer to make
       the rest of this a bit easier. */
    if(c->recvbuf_cur) {
        memcpy(recvbuf, c->recvbuf, c->recvbuf_cur);
        
    }

    /* Attempt to read, and if we don't get anything, punt. */
    if((sz = recv(c->sock, recvbuf + c->recvbuf_cur, 65536 - c->recvbuf_cur,
                  0)) <= 0) {
        if(sz == -1) {
            perror("recv");
        }

        return -1;
    }

    sz += c->recvbuf_cur;
    c->recvbuf_cur = 0;
    rbp = recvbuf;

    /* As long as what we have is long enough, decrypt it. */
    if(sz >= 8) {
        while(sz >= 8 && rv == 0) {
            /* Grab the packet header so we know what exactly we're looking
               for, in terms of packet length. */
            if(!c->hdr_read) {
                if(c->key_set) {
                    RC4(&c->ship_key, 8, rbp, (unsigned char *)&c->pkt);
                }
                else {
                    memcpy(&c->pkt, rbp, 8);
                }

                c->hdr_read = 1;
            }

            pkt_sz = htons(c->pkt.pkt_len);

            /* We'll always need a multiple of 8 bytes. */
            if(pkt_sz & 0x07) {
                pkt_sz = (pkt_sz & 0xFFF8) + 8;
            }

            /* Do we have the whole packet? */
            if(sz >= (ssize_t)pkt_sz) {
                /* Yes, we do, decrypt it. */
                if(c->key_set) {
                    RC4(&c->ship_key, pkt_sz - 8, rbp + 8, rbp + 8);
                }

                memcpy(rbp, &c->pkt, 8);

                /* Pass it onto the correct handler. */
                c->last_message = time(NULL);
                rv = process_ship_pkt(c, (shipgate_hdr_t *)rbp);

                rbp += pkt_sz;
                sz -= pkt_sz;

                c->hdr_read = 0;
            }
            else {
                /* Nope, we're missing part, break out of the loop, and buffer
                   the remaining data. */
                break;
            }
        }
    }

    /* If we've still got something left here, buffer it for the next pass. */
    if(sz) {
        /* Reallocate the recvbuf for the client if its too small. */
        if(c->recvbuf_size < sz) {
            tmp = realloc(c->recvbuf, sz);

            if(!tmp) {
                perror("realloc");
                return -1;
            }

            c->recvbuf = (unsigned char *)tmp;
            c->recvbuf_size = sz;
        }

        memcpy(c->recvbuf, rbp, sz);
        c->recvbuf_cur = sz;
    }
    else {
        /* Free the buffer, if we've got nothing in it. */
        free(c->recvbuf);
        c->recvbuf = NULL;
        c->recvbuf_size = 0;
    }

    return rv;
}
