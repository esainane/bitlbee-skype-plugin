/*
 * Copyright 2012-2013 James Geboski <jgeboski@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include "steam-api.h"
#include "steam-http.h"
#include "steam-util.h"

typedef enum   _SteamApiType SteamApiType;
typedef struct _SteamApiPriv SteamApiPriv;

typedef gboolean (*SteamParseFunc) (SteamApiPriv *priv, json_value *json);

enum _SteamApiType
{
    STEAM_API_TYPE_AUTH = 0,
    STEAM_API_TYPE_FRIENDS,
    STEAM_API_TYPE_LOGON,
    STEAM_API_TYPE_RELOGON,
    STEAM_API_TYPE_LOGOFF,
    STEAM_API_TYPE_MESSAGE,
    STEAM_API_TYPE_POLL,
    STEAM_API_TYPE_SUMMARIES,

    STEAM_API_TYPE_LAST
};

struct _SteamApiPriv
{
    SteamApi     *api;
    SteamApiType  type;
    GError       *err;

    gpointer func;
    gpointer data;

    gpointer       rdata;
    GDestroyNotify rfunc;

    SteamHttpReq *req;
};

static void steam_api_relogon(SteamApi *api);
static const gchar *steam_api_type_str(SteamApiType type);
static SteamMessageType steam_message_type_from_str(const gchar *type);

static SteamApiPriv *steam_api_priv_new(SteamApiType type, SteamApi *api,
                                        gpointer func, gpointer data)
{
    SteamApiPriv *priv;

    priv = g_new0(SteamApiPriv, 1);

    priv->api  = api;
    priv->type = type;
    priv->func = func;
    priv->data = data;

    return priv;
}

static void steam_api_priv_free(SteamApiPriv *priv)
{
    g_return_if_fail(priv != NULL);

    if ((priv->rfunc != NULL) && (priv->rdata != NULL))
        priv->rfunc(priv->rdata);

    if (priv->err != NULL)
        g_error_free(priv->err);

    g_free(priv);
}

GQuark steam_api_error_quark(void)
{
    static GQuark q;

    if (G_UNLIKELY(q == 0))
        q = g_quark_from_static_string("steam-api-error-quark");

    return q;
}

SteamApi *steam_api_new(const gchar *umqid)
{
    SteamApi *api;
    GRand    *rand;

    api = g_new0(SteamApi, 1);

    if (umqid == NULL) {
        rand       = g_rand_new();
        api->umqid = g_strdup_printf("%" G_GUINT32_FORMAT, g_rand_int(rand));

        g_rand_free(rand);
    } else {
        api->umqid = g_strdup(umqid);
    }

    api->http = steam_http_new(STEAM_API_AGENT,
                               (GDestroyNotify) steam_api_priv_free);

    return api;
}

void steam_api_free(SteamApi *api)
{
    g_return_if_fail(api != NULL);

    steam_http_free(api->http);

    g_free(api->token);
    g_free(api->umqid);
    g_free(api->steamid);
    g_free(api);
}

static void steam_slist_free_full(GSList *list)
{
    g_slist_free_full(list, g_free);
}

static gboolean steam_api_auth_cb(SteamApiPriv *priv, json_value *json)
{
    SteamApiError  err;
    const gchar   *str;

    if (steam_util_json_str(json, "access_token", &str)) {
        g_free(priv->api->token);
        priv->api->token = g_strdup(str);
        return TRUE;
    }

    if (steam_util_json_scmp(json, "x_errorcode", "steamguard_code_required",
                             &str))
        err = STEAM_API_ERROR_AUTH_REQ;
    else
        err = STEAM_API_ERROR_AUTH;

    steam_util_json_str(json, "error_description", &str);
    g_set_error(&priv->err, STEAM_API_ERROR, err, "%s", str);
    return TRUE;
}

static gboolean steam_api_friends_cb(SteamApiPriv *priv, json_value *json)
{
    json_value *jv;
    json_value *je;
    GSList     *fl;
    guint       i;

    const gchar *str;

    if (!steam_util_json_val(json, "friends", json_array, &jv))
        goto error;

    fl = NULL;

    for (i = 0; i < jv->u.array.length; i++) {
        je = jv->u.array.values[i];

        if (!steam_util_json_scmp(je, "relationship", "friend", &str))
            continue;

        if (!steam_util_json_str(je, "steamid", &str))
            continue;

        fl = g_slist_prepend(fl, (gchar *) str);
    }

    priv->rdata = fl;
    priv->rfunc = (GDestroyNotify) g_slist_free;

    if (fl != NULL)
        return TRUE;

error:
    g_set_error(&priv->err, STEAM_API_ERROR, STEAM_API_ERROR_FRIENDS,
                "Empty friends list");
    return TRUE;
}

static gboolean steam_api_logon_cb(SteamApiPriv *priv, json_value *json)
{
    const gchar *str;

    if (!steam_util_json_scmp(json, "error", "OK", &str)) {
        g_set_error(&priv->err, STEAM_API_ERROR, STEAM_API_ERROR_LOGON,
                    "%s", str);
        return TRUE;
    }

    steam_util_json_int(json, "message", &priv->api->lmid);

    if (!steam_util_json_scmp(json, "steamid", priv->api->steamid, &str)) {
        g_free(priv->api->steamid);
        priv->api->steamid = g_strdup(str);
    }

    if (!steam_util_json_scmp(json, "umqid", priv->api->umqid, &str)) {
        g_free(priv->api->umqid);
        priv->api->umqid = g_strdup(str);
    }

    return TRUE;
}

static gboolean steam_api_relogon_cb(SteamApiPriv *priv, json_value *json)
{
    const gchar  *str;

    steam_http_queue_pause(priv->api->http, FALSE);

    if (steam_util_json_scmp(json, "error", "OK", &str))
        return TRUE;

    g_set_error(&priv->err, STEAM_API_ERROR, STEAM_API_ERROR_RELOGON,
                "%s", str);
    return TRUE;
}

static gboolean steam_api_logoff_cb(SteamApiPriv *priv, json_value *json)
{
    const gchar *str;

    if (steam_util_json_scmp(json, "error", "OK", &str))
        return TRUE;

    g_set_error(&priv->err, STEAM_API_ERROR, STEAM_API_ERROR_LOGOFF,
                "%s", str);
    return TRUE;
}

static gboolean steam_api_message_cb(SteamApiPriv *priv, json_value *json)
{
    const gchar *str;

    if (steam_util_json_scmp(json, "error", "OK", &str))
        return TRUE;

    if (g_ascii_strcasecmp(str, "Not Logged On") == 0) {
        steam_api_relogon(priv->api);
        steam_http_req_resend(priv->req);
        return FALSE;
    }

    g_set_error(&priv->err, STEAM_API_ERROR, STEAM_API_ERROR_MESSAGE,
                "%s", str);
    return TRUE;
}

static gboolean steam_api_poll_cb(SteamApiPriv *priv, json_value *json)
{
    json_value   *jv;
    json_value   *je;
    GSList       *mu;
    SteamMessage  sm;
    guint         i;
    gint64        in;

    const gchar *str;

    if (steam_util_json_int(json, "messagelast", &in))
        priv->api->lmid = in;

    if (steam_util_json_str(json, "error", &str)  &&
        (g_ascii_strcasecmp(str, "Timeout") != 0) &&
        (g_ascii_strcasecmp(str, "OK")      != 0)) {

        if (g_ascii_strcasecmp(str, "Not Logged On") == 0) {
            steam_api_relogon(priv->api);
            steam_http_req_resend(priv->req);
            return FALSE;
        }

        g_set_error(&priv->err, STEAM_API_ERROR, STEAM_API_ERROR_POLL,
                    "%s", str);
        return TRUE;
    }

    if (!steam_util_json_val(json, "messages", json_array, &jv))
        return TRUE;

    mu = NULL;

    for (i = 0; i < jv->u.array.length; i++) {
        je = jv->u.array.values[i];
        memset(&sm, 0, sizeof sm);

        if (steam_util_json_scmp(je, "steamid_from", priv->api->steamid, &str))
            continue;

        sm.steamid = str;

        if (!steam_util_json_str(je, "type", &str))
            continue;

        sm.type = steam_message_type_from_str(str);

        switch (sm.type) {
        case STEAM_MESSAGE_TYPE_SAYTEXT:
        case STEAM_MESSAGE_TYPE_EMOTE:
            if (!steam_util_json_str(je, "text", &sm.text))
                continue;
            break;

        case STEAM_MESSAGE_TYPE_STATE:
            if (!steam_util_json_str(je, "persona_name", &sm.nick))
                continue;

        case STEAM_MESSAGE_TYPE_RELATIONSHIP:
            if (!steam_util_json_int(je, "persona_state", &in))
                continue;

            sm.state = in;
            break;

        case STEAM_MESSAGE_TYPE_TYPING:
        case STEAM_MESSAGE_TYPE_LEFT_CONV:
            break;

        default:
            continue;
        }

        mu = g_slist_prepend(mu, g_memdup(&sm, sizeof sm));
    }

    priv->rdata = g_slist_reverse(mu);
    priv->rfunc = (GDestroyNotify) steam_slist_free_full;
    return TRUE;
}

static gboolean steam_api_summaries_cb(SteamApiPriv *priv, json_value *json)
{
    json_value   *jv;
    json_value   *je;
    GSList       *mu;
    SteamSummary  ss;
    guint         i;
    gint64        in;

    if (!steam_util_json_val(json, "players", json_array, &jv))
        goto error;

    mu = NULL;

    for (i = 0; i < jv->u.array.length; i++) {
        je = jv->u.array.values[i];
        memset(&ss, 0, sizeof ss);

        if (!steam_util_json_str(je, "steamid", &ss.steamid))
            continue;

        steam_util_json_str(je, "gameextrainfo", &ss.game);
        steam_util_json_str(je, "gameserverip",  &ss.server);
        steam_util_json_str(je, "personaname",   &ss.nick);
        steam_util_json_str(je, "profileurl",    &ss.profile);
        steam_util_json_str(je, "realname",      &ss.fullname);
        steam_util_json_int(je, "personastate",  &in);

        ss.state = in;
        mu = g_slist_prepend(mu, g_memdup(&ss, sizeof ss));
    }

    priv->rdata = mu;
    priv->rfunc = (GDestroyNotify) steam_slist_free_full;

    if (mu != NULL)
        return TRUE;

error:
    g_set_error(&priv->err, STEAM_API_ERROR, STEAM_API_ERROR_SUMMARIES,
                "No friends returned");
    return TRUE;
}

static void steam_api_cb(SteamHttpReq *req, gpointer data)
{
    SteamApiPriv  *priv = data;
    json_value    *json;
    json_settings  js;
    gboolean       callf;
    gchar          err[128];

    static const SteamParseFunc saf[STEAM_API_TYPE_LAST] = {
        [STEAM_API_TYPE_AUTH]      = steam_api_auth_cb,
        [STEAM_API_TYPE_FRIENDS]   = steam_api_friends_cb,
        [STEAM_API_TYPE_LOGON]     = steam_api_logon_cb,
        [STEAM_API_TYPE_RELOGON]   = steam_api_relogon_cb,
        [STEAM_API_TYPE_LOGOFF]    = steam_api_logoff_cb,
        [STEAM_API_TYPE_MESSAGE]   = steam_api_message_cb,
        [STEAM_API_TYPE_POLL]      = steam_api_poll_cb,
        [STEAM_API_TYPE_SUMMARIES] = steam_api_summaries_cb
    };

    if ((priv->type < 0) || (priv->type > STEAM_API_TYPE_LAST))
        return;

    json  = NULL;
    callf = TRUE;

    if (req->err != NULL) {
        g_propagate_error(&priv->err, req->err);
        req->err = NULL;
        goto parse;
    }

    memset(&js, 0, sizeof js);
    json = json_parse_ex(&js, req->body, err);

    if (json == NULL) {
        g_set_error(&priv->err, STEAM_API_ERROR, STEAM_API_ERROR_PARSER,
                    "Parser: %s", err);
    }

parse:
    if ((priv->err == NULL) && (json != NULL)) {
        priv->req = req;
        callf     = saf[priv->type](priv, json);
        priv->req = NULL;
    }

    if (priv->err != NULL)
        g_prefix_error(&priv->err, "%s: ", steam_api_type_str(priv->type));

    if (callf && (priv->func != NULL)) {
        switch (priv->type) {
        case STEAM_API_TYPE_AUTH:
        case STEAM_API_TYPE_LOGON:
        case STEAM_API_TYPE_RELOGON:
        case STEAM_API_TYPE_LOGOFF:
        case STEAM_API_TYPE_MESSAGE:
            ((SteamApiFunc) priv->func)(priv->api, priv->err, priv->data);
            break;

        case STEAM_API_TYPE_FRIENDS:
        case STEAM_API_TYPE_POLL:
        case STEAM_API_TYPE_SUMMARIES:
            ((SteamListFunc) priv->func)(priv->api, priv->rdata, priv->err,
                                         priv->data);
            break;

        default:
            break;
        }
    }

    if (json != NULL)
        json_value_free(json);
}

void steam_api_auth(SteamApi *api, const gchar *authcode,
                    const gchar *user, const gchar *pass,
                    SteamApiFunc func, gpointer data)
{
    SteamHttpReq *req;
    SteamApiPriv *priv;

    g_return_if_fail(api != NULL);

    priv = steam_api_priv_new(STEAM_API_TYPE_AUTH, api, func, data);
    req  = steam_http_req_new(api->http, STEAM_API_HOST, 443,
                              STEAM_API_PATH_AUTH, steam_api_cb, priv);

    steam_http_req_headers_set(req, 1, "User-Agent", STEAM_API_AGENT_AUTH);

    steam_http_req_params_set(req, 8,
        "format",          STEAM_API_FORMAT,
        "client_id",       STEAM_API_CLIENT_ID,
        "grant_type",      "password",
        "username",        user,
        "password",        pass,
        "x_emailauthcode", authcode,
        "x_webcookie",     NULL,
        "scope", "read_profile write_profile read_client write_client"
    );

    req->flags = STEAM_HTTP_REQ_FLAG_POST | STEAM_HTTP_REQ_FLAG_SSL;
    steam_http_req_send(req);
}

void steam_api_friends(SteamApi *api, SteamListFunc func, gpointer data)
{
    SteamHttpReq *req;
    SteamApiPriv *priv;

    g_return_if_fail(api != NULL);

    priv = steam_api_priv_new(STEAM_API_TYPE_FRIENDS, api, func, data);
    req  = steam_http_req_new(api->http, STEAM_API_HOST, 443,
                              STEAM_API_PATH_FRIENDS, steam_api_cb, priv);

    steam_http_req_params_set(req, 4,
        "format",       STEAM_API_FORMAT,
        "access_token", api->token,
        "steamid",      api->steamid,
        "relationship", "friend"
    );

    req->flags = STEAM_HTTP_REQ_FLAG_SSL;
    steam_http_req_send(req);
}

void steam_api_logon(SteamApi *api, SteamApiFunc func, gpointer data)
{
    SteamHttpReq *req;
    SteamApiPriv *priv;

    g_return_if_fail(api != NULL);

    priv = steam_api_priv_new(STEAM_API_TYPE_LOGON, api, func, data);
    req  = steam_http_req_new(api->http, STEAM_API_HOST, 443,
                              STEAM_API_PATH_LOGON, steam_api_cb, priv);

    steam_http_req_params_set(req, 3,
        "format",       STEAM_API_FORMAT,
        "access_token", api->token,
        "umqid",        api->umqid
    );

    req->flags = STEAM_HTTP_REQ_FLAG_POST | STEAM_HTTP_REQ_FLAG_SSL;
    steam_http_req_send(req);
}

static void steam_api_relogon(SteamApi *api)
{
    SteamHttpReq *req;
    SteamApiPriv *priv;

    g_return_if_fail(api != NULL);

    priv = steam_api_priv_new(STEAM_API_TYPE_RELOGON, api, NULL, NULL);
    req  = steam_http_req_new(api->http, STEAM_API_HOST, 443,
                              STEAM_API_PATH_LOGON, steam_api_cb, priv);

    steam_http_req_params_set(req, 3,
        "format",       STEAM_API_FORMAT,
        "access_token", api->token,
        "umqid",        api->umqid
    );

    req->flags = STEAM_HTTP_REQ_FLAG_POST | STEAM_HTTP_REQ_FLAG_SSL;

    steam_http_queue_pause(api->http, TRUE);
    steam_http_req_send(req);
}

void steam_api_logoff(SteamApi *api, SteamApiFunc func, gpointer data)
{
    SteamHttpReq *req;
    SteamApiPriv *priv;

    g_return_if_fail(api != NULL);

    priv = steam_api_priv_new(STEAM_API_TYPE_LOGOFF, api, func, data);
    req  = steam_http_req_new(api->http, STEAM_API_HOST, 443,
                              STEAM_API_PATH_LOGOFF, steam_api_cb, priv);

    steam_http_req_params_set(req, 3,
        "format",       STEAM_API_FORMAT,
        "access_token", api->token,
        "umqid",        api->umqid
    );

    req->flags = STEAM_HTTP_REQ_FLAG_POST | STEAM_HTTP_REQ_FLAG_SSL;
    steam_http_req_send(req);
}

void steam_api_message(SteamApi *api, SteamMessage *sm, SteamApiFunc func,
                       gpointer data)
{
    SteamHttpReq *req;
    SteamApiPriv *priv;

    g_return_if_fail(api != NULL);
    g_return_if_fail(sm  != NULL);

    priv = steam_api_priv_new(STEAM_API_TYPE_MESSAGE, api, func, data);
    req  = steam_http_req_new(api->http, STEAM_API_HOST, 443,
                              STEAM_API_PATH_MESSAGE, steam_api_cb, priv);

    steam_http_req_params_set(req, 5,
        "format",       STEAM_API_FORMAT,
        "access_token", api->token,
        "umqid",        api->umqid,
        "steamid_dst",  sm->steamid,
        "type",         steam_message_type_str(sm->type)
    );

    switch (sm->type) {
    case STEAM_MESSAGE_TYPE_SAYTEXT:
    case STEAM_MESSAGE_TYPE_EMOTE:
        steam_http_req_params_set(req, 1, "text", sm->text);
        break;

    case STEAM_MESSAGE_TYPE_TYPING:
        break;

    default:
        steam_http_req_free(req);
        return;
    }

    req->flags = STEAM_HTTP_REQ_FLAG_QUEUED | STEAM_HTTP_REQ_FLAG_POST |
                 STEAM_HTTP_REQ_FLAG_SSL;

    steam_http_req_send(req);
}

void steam_api_poll(SteamApi *api, SteamListFunc func, gpointer data)
{
    SteamHttpReq *req;
    SteamApiPriv *priv;
    gchar        *lmid;

    g_return_if_fail(api != NULL);

    lmid = g_strdup_printf("%" G_GINT64_FORMAT, api->lmid);
    priv = steam_api_priv_new(STEAM_API_TYPE_POLL, api, func, data);
    req  = steam_http_req_new(api->http, STEAM_API_HOST, 443,
                              STEAM_API_PATH_POLL, steam_api_cb, priv);

    steam_http_req_headers_set(req, 1, "Connection", "Keep-Alive");

    steam_http_req_params_set(req, 5,
        "format",       STEAM_API_FORMAT,
        "access_token", api->token,
        "umqid",        api->umqid,
        "message",      lmid,
        "sectimeout",   STEAM_API_KEEP_ALIVE
    );

    req->flags = STEAM_HTTP_REQ_FLAG_POST | STEAM_HTTP_REQ_FLAG_SSL;
    steam_http_req_send(req);
    g_free(lmid);
}

void steam_api_summaries(SteamApi *api, GSList *friends, SteamListFunc func,
                         gpointer data)
{
    SteamHttpReq *req;
    SteamApiPriv *priv;

    GSList *s;
    GSList *e;
    GSList *l;

    gsize size;
    gint  i;

    gchar *str;
    gchar *p;

    g_return_if_fail(api != NULL);

    if (friends == NULL) {
        if (func != NULL)
            func(api, NULL, NULL, data);

        return;
    }

    s  = friends;

    while (TRUE) {
        size = 0;

        for (l = s, i = 0; (l != NULL) && (i < 100); l = l->next, i++)
            size += strlen(l->data) + 1;

        str = g_new0(gchar, size);
        p   = g_stpcpy(str, s->data);
        e   = l;

        for (l = s->next; l != e; l = l->next) {
            p = g_stpcpy(p, ",");
            p = g_stpcpy(p, l->data);
        }

        priv = steam_api_priv_new(STEAM_API_TYPE_SUMMARIES, api, func, data);
        req  = steam_http_req_new(api->http, STEAM_API_HOST, 443,
                                  STEAM_API_PATH_SUMMARIES, steam_api_cb, priv);

        steam_http_req_params_set(req, 3,
            "format",       STEAM_API_FORMAT,
            "access_token", api->token,
            "steamids",     str
        );

        g_free(str);

        req->flags = STEAM_HTTP_REQ_FLAG_SSL;
        steam_http_req_send(req);

        if (e != NULL)
            s = e->next;
        else
            break;
    }
}

void steam_api_summary(SteamApi *api, const gchar *steamid, SteamListFunc func,
                       gpointer data)
{
    SteamHttpReq *req;
    SteamApiPriv *priv;

    g_return_if_fail(api     != NULL);
    g_return_if_fail(steamid != NULL);

    priv = steam_api_priv_new(STEAM_API_TYPE_SUMMARIES, api, func, data);
    req  = steam_http_req_new(api->http, STEAM_API_HOST, 443,
                              STEAM_API_PATH_SUMMARIES, steam_api_cb, priv);

    steam_http_req_params_set(req, 3,
        "format",       STEAM_API_FORMAT,
        "access_token", api->token,
        "steamids",     steamid
    );

    req->flags = STEAM_HTTP_REQ_FLAG_SSL;
    steam_http_req_send(req);
}

static const gchar *steam_api_type_str(SteamApiType type)
{
    static const gchar *strs[STEAM_API_TYPE_LAST] = {
        [STEAM_API_TYPE_AUTH]      = "Authentication",
        [STEAM_API_TYPE_FRIENDS]   = "Friends",
        [STEAM_API_TYPE_LOGON]     = "Logon",
        [STEAM_API_TYPE_RELOGON]   = "Relogon",
        [STEAM_API_TYPE_LOGOFF]    = "Logoff",
        [STEAM_API_TYPE_MESSAGE]   = "Message",
        [STEAM_API_TYPE_POLL]      = "Polling",
        [STEAM_API_TYPE_SUMMARIES] = "Summaries"
    };

    if ((type < 0) || (type > STEAM_API_TYPE_LAST))
        return "Generic";

    return strs[type];
}

const gchar *steam_message_type_str(SteamMessageType type)
{
    static const gchar *strs[STEAM_MESSAGE_TYPE_LAST] = {
        [STEAM_MESSAGE_TYPE_SAYTEXT]      = "saytext",
        [STEAM_MESSAGE_TYPE_EMOTE]        = "emote",
        [STEAM_MESSAGE_TYPE_LEFT_CONV]    = "leftconversation",
        [STEAM_MESSAGE_TYPE_RELATIONSHIP] = "personarelationship",
        [STEAM_MESSAGE_TYPE_STATE]        = "personastate",
        [STEAM_MESSAGE_TYPE_TYPING]       = "typing"
    };

    if ((type < 0) || (type > STEAM_MESSAGE_TYPE_LAST))
        return "";

    return strs[type];
}

const gchar *steam_state_str(SteamState state)
{
    static const gchar *strs[STEAM_STATE_LAST] = {
        [STEAM_STATE_OFFLINE] = "Offline",
        [STEAM_STATE_ONLINE]  = "Online",
        [STEAM_STATE_BUSY]    = "Busy",
        [STEAM_STATE_AWAY]    = "Away",
        [STEAM_STATE_SNOOZE]  = "Snooze"
    };

    if ((state < 0) || (state > STEAM_STATE_LAST))
        return "";

    return strs[state];
}

static SteamMessageType steam_message_type_from_str(const gchar *type)
{
    const gchar *s;
    guint        i;

    if (type == NULL)
        return STEAM_MESSAGE_TYPE_LAST;

    for (i = 0; i < STEAM_MESSAGE_TYPE_LAST; i++) {
        s = steam_message_type_str(i);

        if (g_ascii_strcasecmp(type, s) == 0)
            return i;
    }

    return STEAM_MESSAGE_TYPE_LAST;
}

SteamState steam_state_from_str(const gchar *state)
{
    const gchar *s;
    guint        i;

    if (state == NULL)
        return STEAM_STATE_OFFLINE;

    for (i = 0; i < STEAM_STATE_LAST; i++) {
        s = steam_state_str(i);

        if (g_ascii_strcasecmp(state, s) == 0)
            return i;
    }

    return STEAM_STATE_OFFLINE;
}
