// Copyright 2021 The NATS Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <ctype.h>

#include "natsp.h"
#include "kv.h"
#include "mem.h"
#include "util.h"
#include "js.h"

static const char *kvBucketNameTmpl  = "KV_%s";
static const char *kvSubjectsTmpl    = "$KV.%s.>";
static const char *kvSubjectsPreTmpl = "$KV.%s.";
// static const char *kvNoPending       = "0";

#define KV_WATCH_FOR_EVER (int64_t)(0x7FFFFFFFFFFFFFFF)

#define DEFINE_BUF_FOR_SUBJECT \
char        buffer[128];    \
natsBuffer  buf;

#define BUILD_SUBJECT \
natsBuf_InitWithBackend(&buf, buffer, 0, sizeof(buffer));   \
s = natsBuf_Append(&buf, kv->pre, -1);                      \
IFOK(s, natsBuf_Append(&buf, key, -1));                     \
IFOK(s, natsBuf_AppendByte(&buf, 0));

#define KV_DEFINE_LIST \
kvEntry         *e = NULL;                  \
kvEntry         *h = NULL;                  \
kvEntry         *t = NULL;                  \
int             n  = 0;                     \
int64_t         timeout = KV_WATCH_FOR_EVER;\
int64_t         start;                      \
int             i;

#define KV_GATHER_LIST \
start = nats_Now();                                 \
while (s == NATS_OK)                                \
{                                                   \
    s = kvWatcher_Next(&e, w, timeout);             \
    if (s == NATS_OK)                               \
    {                                               \
        if (e == NULL)                              \
            break;                                  \
        if (t != NULL)                              \
            t->next = e;                            \
        else                                        \
            h = e;                                  \
        t = e;                                      \
        n++;                                        \
        timeout -= (nats_Now() - start);            \
        if (timeout <= 0)                           \
            s = nats_setDefaultError(NATS_TIMEOUT); \
    }                                               \
}

//////////////////////////////////////////////////////////////////////////////
// kvStore management APIs
//////////////////////////////////////////////////////////////////////////////

natsStatus
kvConfig_Init(kvConfig *cfg)
{
    if (cfg == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    memset(cfg, 0, sizeof(kvConfig));
    return NATS_OK;
}

static bool
validBucketName(const char *bucket)
{
    int     i;
    char    c;

    if (nats_IsStringEmpty(bucket))
        return false;

    for (i=0; i<(int)strlen(bucket); i++)
    {
        c = bucket[i];
        if ((isalnum(c) == 0) && (c != '_') && (c != '-'))
            return false;
    }
    return true;
}

static void
_freeKV(kvStore *kv)
{
    jsCtx *js = NULL;

    if (kv == NULL)
        return;

    js = kv->js;
    NATS_FREE(kv->bucket);
    NATS_FREE(kv->stream);
    NATS_FREE(kv->pre);
    natsMutex_Destroy(kv->mu);
    NATS_FREE(kv);
    js_release(js);
}

static void
_retainKV(kvStore *kv)
{
    natsMutex_Lock(kv->mu);
    kv->refs++;
    natsMutex_Unlock(kv->mu);
}

static void
_releaseKV(kvStore *kv)
{
    bool doFree;

    if (kv == NULL)
        return;

    natsMutex_Lock(kv->mu);
    doFree = (--(kv->refs) == 0);
    natsMutex_Unlock(kv->mu);

    if (doFree)
        _freeKV(kv);
}

void
kvStore_Destroy(kvStore *kv)
{
    _releaseKV(kv);
}

static natsStatus
_createKV(kvStore **new_kv, jsCtx *js, const char *bucket)
{
    natsStatus  s   = NATS_OK;
    kvStore     *kv = NULL;

    if (!validBucketName(bucket))
        return nats_setError(NATS_INVALID_ARG, "%s", kvErrInvalidBucketName);

    kv = (kvStore*) NATS_CALLOC(1, sizeof(kvStore));
    if (kv == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    kv->refs = 1;
    s = natsMutex_Create(&(kv->mu));
    IF_OK_DUP_STRING(s, kv->bucket, bucket);
    if ((s == NATS_OK) && (nats_asprintf(&(kv->stream), kvBucketNameTmpl, bucket) < 0))
        s = nats_setDefaultError(NATS_NO_MEMORY);
    if ((s == NATS_OK) && (nats_asprintf(&(kv->pre), kvSubjectsPreTmpl, bucket) < 0))
        s = nats_setDefaultError(NATS_NO_MEMORY);

    if (s == NATS_OK)
    {
        kv->js = js;
        js_retain(js);
        *new_kv = kv;
    }
    else
        _freeKV(kv);

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
js_CreateKeyValue(kvStore **new_kv, jsCtx *js, kvConfig *cfg)
{
    natsStatus      s;
    int64_t         history = 1;
    int64_t         replicas= 1;
    kvStore         *kv     = NULL;
    char            *subject= NULL;
    jsStreamConfig  sc;

    if ((new_kv == NULL) || (js == NULL) || (cfg == NULL))
        return nats_setDefaultError(NATS_INVALID_ARG);

    s = _createKV(&kv, js, cfg->Bucket);
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    if (cfg->History > 0)
    {
        if (cfg->History > kvMaxHistory)
            s = nats_setError(NATS_INVALID_ARG, "%s %d", kvErrHistoryTooLarge, kvMaxHistory);
        else
            history = (int64_t) cfg->History;
    }
    if (s == NATS_OK)
    {
        if (cfg->Replicas > 0)
            replicas = cfg->Replicas;

        if (nats_asprintf(&subject, kvSubjectsTmpl, kv->bucket) < 0)
            s = nats_setDefaultError(NATS_NO_MEMORY);
    }
    if (s == NATS_OK)
    {
        jsStreamConfig_Init(&sc);
        sc.Name = kv->stream;
        sc.Description = cfg->Description;
        sc.Subjects = (const char*[1]){subject};
        sc.SubjectsLen = 1;
        sc.MaxMsgsPerSubject = history;
        sc.MaxBytes = cfg->MaxBytes;
        sc.MaxAge = cfg->TTL;
        sc.MaxMsgSize = cfg->MaxValueSize;
        sc.Storage = cfg->StorageType;
        sc.Replicas = replicas;
        sc.AllowRollup = true;
        sc.DenyDelete = true;

        s = js_AddStream(NULL, js, &sc, NULL, NULL);
    }
    if (s == NATS_OK)
        *new_kv = kv;
    else
        _freeKV(kv);

    NATS_FREE(subject);

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
js_KeyValue(kvStore **new_kv, jsCtx *js, const char *bucket)
{
    natsStatus      s;
    kvStore         *kv     = NULL;
    jsStreamInfo    *si     = NULL;

    if ((new_kv == NULL) || (js == NULL))
        return nats_setDefaultError(NATS_INVALID_ARG);

    s = _createKV(&kv, js, bucket);
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    s = js_GetStreamInfo(&si, js, kv->stream, NULL, NULL);
    if (s == NATS_OK)
    {
        // Do some quick sanity checks that this is a correctly formed stream for KV.
        // Max msgs per subject should be > 0.
        if (si->Config->MaxMsgsPerSubject < 1)
            s = nats_setError(NATS_INVALID_ARG, "%s", kvErrBadBucket);

        jsStreamInfo_Destroy(si);
    }

    if (s == NATS_OK)
        *new_kv = kv;
    else
    {
        _freeKV(kv);
        if (s == NATS_NOT_FOUND)
            return s;
    }

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
js_DeleteKeyValue(jsCtx *js, const char *bucket)
{
    natsStatus  s;
    char        *stream = NULL;

    if (js == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    if (!validBucketName(bucket))
        return nats_setError(NATS_INVALID_ARG, "%s", kvErrBadBucket);

    if (nats_asprintf(&stream, kvBucketNameTmpl, bucket) < 0)
        return nats_setDefaultError(NATS_NO_MEMORY);

    s = js_DeleteStream(js, (const char*) stream, NULL, NULL);

    NATS_FREE(stream);

    return NATS_UPDATE_ERR_STACK(s);
}

//////////////////////////////////////////////////////////////////////////////
// kvStore APIs
//////////////////////////////////////////////////////////////////////////////

static bool
validKey(const char *key)
{
    int     i;
    char    c;
    int     last;

    if (nats_IsStringEmpty(key))
        return false;

    last = (int) strlen(key);
    for (i=0; i<last; i++)
    {
        c = key[i];
        if ((c == '.') && ((i == 0) || (i == last-1) || (key[i-1] == '.')))
        {
            return false;
        }
        else if ((isalnum(c) == 0) && (c != '.') && (c != '_') && (c != '-')
                    && (c != '/') && (c != '\\') && (c != '='))
        {
            return false;
        }
    }
    return true;
}

static natsStatus
_createEntry(kvEntry **new_entry, kvStore *kv, natsMsg **msg)
{
    kvEntry     *e   = NULL;
    const char  *val = NULL;

    e = (kvEntry*) NATS_CALLOC(1, sizeof(kvEntry));
    if (e == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    _retainKV(kv);
    e->kv  = kv;
    e->msg = *msg;
    e->key = e->msg->subject+(int)strlen(kv->pre);

    if (natsMsgHeader_Get(e->msg, kvOpHeader, &val) == NATS_OK)
    {
        if (strcmp(val, kvOpDeleteStr) == 0)
            e->op = kvOp_Delete;
        else if (strcmp(val, kvOpPurgeStr) == 0)
            e->op = kvOp_Purge;
    }

    // Indicate that we took ownership of the message
    *msg = NULL;
    *new_entry = e;

    return NATS_OK;
}

static natsStatus
_getEntry(kvEntry **new_entry, bool *deleted, kvStore *kv, const char *key)
{
    natsStatus  s       = NATS_OK;
    natsMsg     *msg    = NULL;
    kvEntry     *e      = NULL;
    DEFINE_BUF_FOR_SUBJECT;

    *new_entry = NULL;
    *deleted   = false;

    if (!validKey(key))
        return nats_setError(NATS_INVALID_ARG, "%s", kvErrInvalidKey);

    BUILD_SUBJECT;
    IFOK(s, js_GetLastMsg(&msg, kv->js, kv->stream, natsBuf_Data(&buf), NULL, NULL));
    IFOK(s, _createEntry(&e, kv, &msg));

    natsBuf_Cleanup(&buf);
    natsMsg_Destroy(msg);

    if (s == NATS_OK)
    {
        if ((e->op == kvOp_Delete) || (e->op == kvOp_Purge))
            *deleted = true;
        *new_entry = e;
    }
    else
    {
        kvEntry_Destroy(e);

        if (s == NATS_NOT_FOUND)
        {
            nats_clearLastError();
            return s;
        }
    }
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
kvStore_Get(kvEntry **new_entry, kvStore *kv, const char *key)
{
    natsStatus  s;
    bool        deleted = false;

    if ((new_entry == NULL) || (kv == NULL))
        return nats_setDefaultError(NATS_INVALID_ARG);

    s = _getEntry(new_entry, &deleted, kv, key);
    if (s == NATS_OK)
    {
        if (deleted)
        {
            kvEntry_Destroy(*new_entry);
            *new_entry = NULL;
            return NATS_NOT_FOUND;
        }
    }
    else if (s == NATS_NOT_FOUND)
        return s;

    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_putEntry(uint64_t *rev, kvStore *kv, jsPubOptions *po, const char *key, const void *data, int len)
{
    natsStatus  s       = NATS_OK;
    jsPubAck    *pa     = NULL;
    jsPubAck    **ppa   = NULL;
    DEFINE_BUF_FOR_SUBJECT;

    if (rev != NULL)
    {
        *rev = 0;
        ppa = &pa;
    }

    if (kv == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    if (!validKey(key))
        return nats_setError(NATS_INVALID_ARG, "%s", kvErrInvalidKey);

    BUILD_SUBJECT;
    IFOK(s, js_Publish(ppa, kv->js, natsBuf_Data(&buf), data, len, po, NULL));

    if ((s == NATS_OK) && (rev != NULL))
        *rev = pa->Sequence;

    natsBuf_Cleanup(&buf);
    jsPubAck_Destroy(pa);

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
kvStore_Put(uint64_t *rev, kvStore *kv, const char *key, const void *data, int len)
{
    natsStatus s;

    s = _putEntry(rev, kv, NULL, key, data, len);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
kvStore_PutString(uint64_t *rev, kvStore *kv, const char *key, const char *data)
{
    natsStatus  s;
    int         l = (data == NULL ? 0 : (int) strlen(data));

    s = kvStore_Put(rev, kv, key, (const void*) data, l);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
kvStore_Create(uint64_t *rev, kvStore *kv, const char *key, const void *data, int len)
{
    natsStatus s;
    natsStatus ls;
    kvEntry    *e = NULL;
    bool       deleted = false;

    if (kv == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    s = kvStore_Update(rev, kv, key, data, len, 0);
    if (s == NATS_OK)
        return s;

    // Since we have tombstones for DEL ops for watchers, this could be from that
    // so we need to double check.
    ls = _getEntry(&e, &deleted, kv, key);
    if (ls == NATS_OK)
    {
        if (deleted)
            s = kvStore_Update(rev, kv, key, data, len, kvEntry_Revision(e));

        kvEntry_Destroy(e);
    }
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
kvStore_CreateString(uint64_t *rev, kvStore *kv, const char *key, const char *data)
{
    natsStatus s = kvStore_Create(rev, kv, key, (const void*) data, (int) strlen(data));
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
kvStore_Update(uint64_t *rev, kvStore *kv, const char *key, const void *data, int len, uint64_t last)
{
    natsStatus      s;
    jsPubOptions    po;

    jsPubOptions_Init(&po);
    if (last == 0)
        po.ExpectNoMessage = true;
    else
        po.ExpectLastSubjectSeq = last;
    s = _putEntry(rev, kv, &po, key, data, len);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
kvStore_UpdateString(uint64_t *rev, kvStore *kv, const char *key, const char *data, uint64_t last)
{
    natsStatus s = kvStore_Update(rev, kv, key, (const void*) data, (int) strlen(data), last);
    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_delete(kvStore *kv, const char *key, bool purge)
{
    natsStatus  s;
    natsMsg     *msg = NULL;
    DEFINE_BUF_FOR_SUBJECT;

    if (kv == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    if (!validKey(key))
        return nats_setError(NATS_INVALID_ARG, "%s", kvErrInvalidKey);

    BUILD_SUBJECT;
    IFOK(s, natsMsg_Create(&msg, natsBuf_Data(&buf), NULL, NULL, 0));
    if (s == NATS_OK)
    {
        if (purge)
        {
            s = natsMsgHeader_Set(msg, kvOpHeader, kvOpPurgeStr);
            IFOK(s, natsMsgHeader_Set(msg, JSMsgRollup, JSMsgRollupSubject));
        }
        else
        {
            s = natsMsgHeader_Set(msg, kvOpHeader, kvOpDeleteStr);
        }
    }
    IFOK(s, js_PublishMsg(NULL, kv->js, msg, NULL, NULL));

    natsBuf_Cleanup(&buf);
    natsMsg_Destroy(msg);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
kvStore_Delete(kvStore *kv, const char *key)
{
    natsStatus s = _delete(kv, key, false);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
kvStore_Purge(kvStore *kv, const char *key)
{
    natsStatus s = _delete(kv, key, true);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
kvStore_PurgeDeletes(kvStore *kv, kvWatchOptions *opts)
{
    natsStatus      s;
    kvWatcher       *w = NULL;
    kvEntry         *e = NULL;
    kvEntry         *h = NULL;
    kvEntry         *t = NULL;
    natsBuffer      buf;
    char            buffer[128];

    s = kvStore_WatchAll(&w, kv, opts);
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    while (s == NATS_OK)
    {
        s = kvWatcher_Next(&e, w, KV_WATCH_FOR_EVER);
        if (s == NATS_OK)
        {
            if (e == NULL)
                break;
            if ((e->op == kvOp_Delete) || (e->op == kvOp_Purge))
            {
                if (t != NULL)
                    t->next = e;
                else
                    h = e;
                t = e;
            }
        }
    }
    if ((s == NATS_OK) && (h != NULL))
    {
        jsOptions po;

        jsOptions_Init(&po);

        natsBuf_InitWithBackend(&buf, buffer, 0, sizeof(buffer));

        for (; h != NULL; )
        {
            natsBuf_Reset(&buf);
            IFOK(s, natsBuf_Append(&buf, kv->pre, -1));
            IFOK(s, natsBuf_Append(&buf, h->key, -1));
            IFOK(s, natsBuf_AppendByte(&buf, '\0'));
            if (s == NATS_OK)
            {
                po.Stream.Purge.Subject = (const char*) natsBuf_Data(&buf);
                s = js_PurgeStream(kv->js, kv->stream, &po, NULL);
            }
            e = h;
            h = h->next;
            kvEntry_Destroy(e);
        }
    }
    kvWatcher_Destroy(w);
    return NATS_UPDATE_ERR_STACK(s);
}

static void
_freeWatcher(kvWatcher *w)
{
    kvStore *kv = NULL;

    natsSubscription_Destroy(w->sub);
    natsMutex_Destroy(w->mu);
    kv = w->kv;
    NATS_FREE(w);
    _releaseKV(kv);
}

static void
_releaseWatcher(kvWatcher *w)
{
    bool doFree;

    if (w == NULL)
        return;

    natsMutex_Lock(w->mu);
    doFree = (--(w->refs) == 0);
    natsMutex_Unlock(w->mu);

    if (doFree)
        _freeWatcher(w);
}

natsStatus
kvWatchOptions_Init(kvWatchOptions *opts)
{
    if (opts == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    memset(opts, 0, sizeof(kvWatchOptions));
    return NATS_OK;
}

natsStatus
kvWatcher_Next(kvEntry **new_entry, kvWatcher *w, int64_t timeout)
{
    natsStatus  s    = NATS_OK;
    kvEntry     *e   = NULL;
    int64_t     start;

    if ((new_entry == NULL) || (w == NULL) || (timeout <= 0))
        return nats_setDefaultError(NATS_INVALID_ARG);

    *new_entry = NULL;

    natsMutex_Lock(w->mu);
    start = nats_Now();
GET_NEXT:
    if (w->stopped)
    {
        s = nats_setDefaultError(NATS_ILLEGAL_STATE);
    }
    else if (w->retMarker)
    {
        // Will return a NULL entry (*new_entry is initialized to NULL).
        // Mark that we should no longer check/return the "init done" marker.
        w->retMarker = false;
    }
    else
    {
        natsMsg     *msg = NULL;
        uint64_t    delta= 0;

        w->refs++;
        natsMutex_Unlock(w->mu);

        s = natsSubscription_NextMsg(&msg, w->sub, timeout);

        natsMutex_Lock(w->mu);
        if (w->stopped)
        {
            natsMutex_Unlock(w->mu);
            _releaseWatcher(w);
            return NATS_ILLEGAL_STATE;
        }
        w->refs--;

        if ((s == NATS_OK) && (strlen(msg->subject) <= strlen(w->kv->pre)))
            s = nats_setError(NATS_ERR, "invalid update's subject '%s'", msg->subject);

        if ((s == NATS_OK) && ((nats_IsStringEmpty(msg->reply) ||
                                ((int) strlen(msg->reply) <= jsAckPrefixLen))))
        {
            s = nats_setError(NATS_ERR, "unable to get metadata from '%s'", msg->reply);
        }
        IFOK(s, js_getMetaData(msg->reply+jsAckPrefixLen,
                               NULL, NULL, NULL, NULL, &(msg->seq),
                               NULL, &(msg->time), &delta, 3));
        IFOK(s, _createEntry(&e, w->kv, &msg));
        if (s == NATS_OK)
        {
            e->delta = delta;

            // Check if done with initial values
            if (!w->initDone && (delta == 0))
            {
                w->initDone = true;
                w->retMarker = true;
            }
            if (w->ignoreDel && ((e->op == kvOp_Delete) || e->op == kvOp_Purge))
            {
                kvEntry_Destroy(e);
                e = NULL;
                timeout -= (nats_Now() - start);
                if (timeout > 0)
                    goto GET_NEXT;
                else
                    s = nats_setDefaultError(NATS_TIMEOUT);
            }
        }
        natsMsg_Destroy(msg);
    }
    natsMutex_Unlock(w->mu);

    if (s == NATS_OK)
        *new_entry = e;

    return NATS_UPDATE_ERR_STACK(s);
}

void
kvWatcher_Destroy(kvWatcher *w)
{
    kvWatcher_Stop(w);
    _releaseWatcher(w);
}

natsStatus
kvStore_Watch(kvWatcher **new_watcher, kvStore *kv, const char *key, kvWatchOptions *opts)
{
    natsStatus      s;
    kvWatcher       *w = NULL;
    jsSubOptions    so;
    DEFINE_BUF_FOR_SUBJECT;

    if ((new_watcher == NULL) || (kv == NULL) || nats_IsStringEmpty(key))
        return nats_setDefaultError(NATS_INVALID_ARG);

    *new_watcher = NULL;

    w = (kvWatcher*) NATS_CALLOC(1, sizeof(kvWatcher));
    if (w == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    _retainKV(kv);
    w->kv = kv;
    w->refs = 1;

    BUILD_SUBJECT;
    IFOK(s, natsMutex_Create(&(w->mu)));
    if (s == NATS_OK)
    {
        natsStatus  ls;
        natsMsg     *msg = NULL;

        // Check if we have anything pending.
        ls = js_GetLastMsg(&msg, kv->js, kv->stream, natsBuf_Data(&buf), NULL, NULL);
        if (ls == NATS_OK)
            natsMsg_Destroy(msg);
        else
        {
            nats_clearLastError();
            if (ls == NATS_NOT_FOUND)
            {
                w->initDone  = true;
                w->retMarker = true;
            }
        }
    }
    // Use ordered consumer to deliver results
    if (s == NATS_OK)
    {
        jsSubOptions_Init(&so);
        so.Ordered = true;
        if ((opts == NULL) || !opts->IncludeHistory)
            so.Config.DeliverPolicy = js_DeliverLastPerSubject;
        if (opts != NULL)
        {
            if (opts->MetaOnly)
                so.Config.HeadersOnly = true;
            if (opts->IgnoreDeletes)
                w->ignoreDel = true;
        }
        s = js_SubscribeSync(&(w->sub), kv->js, natsBuf_Data(&buf), NULL, &so, NULL);
    }

    natsBuf_Cleanup(&buf);

    if (s == NATS_OK)
        *new_watcher = w;
    else
        _freeWatcher(w);

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
kvStore_WatchAll(kvWatcher **new_watcher, kvStore *kv, kvWatchOptions *opts)
{
    natsStatus s = kvStore_Watch(new_watcher, kv, ">", opts);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
kvStore_Keys(kvKeysList *list, kvStore *kv, kvWatchOptions *opts)
{
    natsStatus      s;
    kvWatchOptions  o;
    kvWatcher       *w = NULL;
    int             count = 0;
    KV_DEFINE_LIST;

    if (list == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    list->Keys = NULL;
    *(int*)&(list->Count) = 0;

    kvWatchOptions_Init(&o);
    if (opts != NULL)
        memcpy(&o, opts, sizeof(kvWatchOptions));

    o.IgnoreDeletes = true;
    o.MetaOnly = true;
    if (o.Timeout > 0)
        timeout = o.Timeout;

    s = kvStore_WatchAll(&w, kv, &o);
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    KV_GATHER_LIST;

    // Don't need the watcher anymore.
    kvWatcher_Destroy(w);
    // On success, create the array of keys.
    if ((s == NATS_OK) && (n > 0))
    {
        list->Keys = (char**) NATS_CALLOC(n, sizeof(char*));
        if (list->Keys == NULL)
            s = nats_setDefaultError(NATS_NO_MEMORY);
    }
    // Transfer keys to the array (on success), and destroy
    // the entries if there was an error.
    for (i=0; h != NULL; i++)
    {
        e = h;
        h = h->next;
        if (s == NATS_OK)
        {
            DUP_STRING(s, list->Keys[i], e->key);
            if (s == NATS_OK)
                count++;
        }
        kvEntry_Destroy(e);
    }
    // Set the list's Count to `count`, not `n` since `count`
    // will reflect the actual number of keys that have been
    // properly strdup'ed.
    *(int*)&(list->Count) = count;

    // If there was a failure (especially when strdup'ing) keys,
    // this will do the proper cleanup and re-initialize the list.
    if (s != NATS_OK)
        kvKeysList_Destroy(list);

    return NATS_UPDATE_ERR_STACK(s);
}

void
kvKeysList_Destroy(kvKeysList *list)
{
    int i;

    if ((list == NULL) || (list->Keys == NULL))
        return;

    for (i=0; i<list->Count; i++)
        NATS_FREE(list->Keys[i]);
    NATS_FREE(list->Keys);
    list->Keys = NULL;
    *(int*)&(list->Count) = 0;
}

natsStatus
kvStore_History(kvEntryList *list, kvStore *kv, const char *key, kvWatchOptions *opts)
{
    natsStatus      s;
    kvWatchOptions  o;
    kvEntry         *e = NULL;
    kvEntry         *h = NULL;
    kvEntry         *t = NULL;
    int             n  = 0;
    kvWatcher       *w = NULL;
    int64_t         timeout = KV_WATCH_FOR_EVER;
    int64_t         start;
    int             i;

    if (list == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    list->Entries = NULL;
    *(int*)&(list->Count) = 0;

    kvWatchOptions_Init(&o);
    if (opts != NULL)
        memcpy(&o, opts, sizeof(kvWatchOptions));

    o.IncludeHistory = true;
    if (o.Timeout > 0)
        timeout = o.Timeout;

    s = kvStore_Watch(&w, kv, key, &o);
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    KV_GATHER_LIST;

    // Don't need the watcher anymore.
    kvWatcher_Destroy(w);
    // On success, create the array of entries.
    if ((s == NATS_OK) && (n > 0))
    {
        list->Entries = (kvEntry**) calloc(n, sizeof(kvEntry*));
        if (list->Entries == NULL)
            s = nats_setDefaultError(NATS_NO_MEMORY);
        else
            *(int*)&(list->Count) = n;
    }
    // Transfer entries to the array (on success), or destroy
    // the entries if there was an error.
    for (i=0; h != NULL; i++)
    {
        e = h;
        h = h->next;
        if (s == NATS_OK)
            list->Entries[i] = e;
        else
            kvEntry_Destroy(e);
    }
    return NATS_UPDATE_ERR_STACK(s);
}

void
kvEntryList_Destroy(kvEntryList *list)
{
    int i;

    if ((list == NULL) || (list->Entries == NULL))
        return;

    for (i=0; i<list->Count; i++)
        kvEntry_Destroy(list->Entries[i]);
    NATS_FREE(list->Entries);
    list->Entries = NULL;
    *(int*)&(list->Count) = 0;
}

natsStatus
kvWatcher_Stop(kvWatcher *w)
{
    natsStatus s = NATS_OK;

    if (w == NULL)
        return NATS_INVALID_ARG;

    natsMutex_Lock(w->mu);
    if (!w->stopped)
    {
        w->stopped = true;
        s = natsSubscription_Unsubscribe(w->sub);
    }
    natsMutex_Unlock(w->mu);

    return NATS_UPDATE_ERR_STACK(s);
}

const char*
kvStore_Bucket(kvStore *kv)
{
    return (kv == NULL ? NULL : kv->bucket);
}

natsStatus
kvStore_Status(kvStatus **new_status, kvStore *kv)
{
    natsStatus      s;
    kvStatus        *sts = NULL;
    jsStreamInfo    *si  = NULL;

    if ((new_status == NULL) || (kv == NULL))
        return nats_setDefaultError(NATS_INVALID_ARG);

    s = js_GetStreamInfo(&si, kv->js, kv->stream, NULL, NULL);
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    sts = (kvStatus*) NATS_CALLOC(1, sizeof(kvStatus));
    if (sts == NULL)
        s = nats_setDefaultError(NATS_NO_MEMORY);

    if (s == NATS_OK)
    {
        _retainKV(kv);
        sts->kv = kv;
        sts->si = si;
        *new_status = sts;
    }
    else
        jsStreamInfo_Destroy(si);

    return NATS_UPDATE_ERR_STACK(s);
}

//////////////////////////////////
// kvStatus APIs
//////////////////////////////////

const char*
kvStatus_Bucket(kvStatus *sts)
{
    return (sts == NULL ? NULL : sts->kv->bucket);
}

uint64_t
kvStatus_Values(kvStatus *sts)
{
    return (sts == NULL ? 0 : sts->si->State.Msgs);
}

int64_t
kvStatus_History(kvStatus *sts)
{
    return (sts == NULL || sts->si->Config == NULL ? 0 : sts->si->Config->MaxMsgsPerSubject);
}

int64_t
kvStatus_TTL(kvStatus *sts)
{
    return (sts == NULL || sts->si->Config == NULL ? 0 : sts->si->Config->MaxAge);
}

int64_t
kvStatus_Replicas(kvStatus *sts)
{
    return (sts == NULL || sts->si->Config == NULL ? 0 : sts->si->Config->Replicas);
}

void
kvStatus_Destroy(kvStatus *sts)
{
    kvStore *kv = NULL;

    if (sts == NULL)
        return;

    kv = sts->kv;
    jsStreamInfo_Destroy(sts->si);
    NATS_FREE(sts);
    _releaseKV(kv);
}

//////////////////////////////////
// kvEntry APIs
//////////////////////////////////

const char*
kvEntry_Bucket(kvEntry *e)
{
    return (e == NULL ? NULL : kvStore_Bucket(e->kv));
}

const char*
kvEntry_Key(kvEntry *e)
{
    return (e == NULL ? NULL : e->key);
}

const void*
kvEntry_Value(kvEntry *e)
{
    return (e == NULL ? NULL : (const void*) natsMsg_GetData(e->msg));
}

int
kvEntry_ValueLen(kvEntry *e)
{
    return (e == NULL ? -1 : natsMsg_GetDataLength(e->msg));
}

const char*
kvEntry_ValueString(kvEntry *e)
{
    return (e == NULL ? NULL : natsMsg_GetData(e->msg));
}

uint64_t
kvEntry_Revision(kvEntry *e)
{
    return (e == NULL ? 0 : natsMsg_GetSequence(e->msg));
}

int64_t
kvEntry_Created(kvEntry *e)
{
    return (e == NULL ? 0 : natsMsg_GetTime(e->msg));
}

uint64_t
kvEntry_Delta(kvEntry *e)
{
    return (e == NULL ? 0 : e->delta);
}

kvOperation
kvEntry_Operation(kvEntry *e)
{
    return (e == NULL ? 0 : e->op);
}

void
kvEntry_Destroy(kvEntry *e)
{
    kvStore *kv = NULL;

    if (e == NULL)
        return;

    kv = e->kv;
    natsMsg_Destroy(e->msg);
    NATS_FREE(e);
    _releaseKV(kv);
}
