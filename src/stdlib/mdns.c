/*
 * mdns.c — Implementation of the std.mdns standard library module.
 *
 * macOS: full implementation using the Bonjour dns_sd API (dns_sd.h).
 * Linux/Windows: stub implementation; mdns_is_available() returns 0 and all
 *   other functions return safe no-op values.
 *
 * Thread safety: all public functions are protected by g_mdns_mutex.
 * Browse sessions each run a background POSIX thread (macOS only) that loops
 * over DNSServiceProcessResult until the session is cancelled.
 *
 * Story: 72.8
 */

#include "mdns.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* =========================================================================
 * macOS — full Bonjour implementation
 * ========================================================================= */

#ifdef __APPLE__

#include <dns_sd.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

/* ---------------------------------------------------------------------- */
/* Internal limits                                                          */
/* ---------------------------------------------------------------------- */

#define MDNS_MAX_REGISTRATIONS 32
#define MDNS_MAX_BROWSES       32
#define MDNS_MAX_TXT_ENTRIES   64
#define MDNS_MAX_TXT_ENTRY_LEN 255

/* ---------------------------------------------------------------------- */
/* Internal registration record                                             */
/* ---------------------------------------------------------------------- */

typedef struct {
    int            active;
    char           name[256];
    DNSServiceRef  sdref;
} MdnsRegistration;

/* ---------------------------------------------------------------------- */
/* Internal browse record                                                   */
/* ---------------------------------------------------------------------- */

typedef struct {
    int              active;
    char             service_type[128];
    DNSServiceRef    sdref;
    MdnsBrowseCallback cb;
    void            *user_data;
    pthread_t        thread;
    volatile int     stop_flag; /* set to 1 to request thread exit */
} MdnsBrowseSession;

/* ---------------------------------------------------------------------- */
/* Global state                                                             */
/* ---------------------------------------------------------------------- */

static pthread_mutex_t   g_mdns_mutex        = PTHREAD_MUTEX_INITIALIZER;
static MdnsRegistration  g_regs[MDNS_MAX_REGISTRATIONS];
static MdnsBrowseSession g_browses[MDNS_MAX_BROWSES];
static int               g_state_initialized = 0;

/* ---------------------------------------------------------------------- */
/* Helpers                                                                  */
/* ---------------------------------------------------------------------- */

static void state_init_locked(void)
{
    if (!g_state_initialized) {
        memset(g_regs,    0, sizeof(g_regs));
        memset(g_browses, 0, sizeof(g_browses));
        g_state_initialized = 1;
    }
}

/*
 * build_txt_record — convert an array of "key=value" strings into a
 * TXTRecordRef.  Caller must call TXTRecordDeallocate when done.
 */
static void build_txt_record(const MdnsTxtArray *txt, TXTRecordRef *out)
{
    TXTRecordCreate(out, 0, NULL);

    if (!txt || txt->len == 0 || !txt->data)
        return;

    for (uint64_t i = 0; i < txt->len; i++) {
        const char *entry = txt->data[i];
        if (!entry)
            continue;

        /* Split on first '=' */
        const char *eq = strchr(entry, '=');
        if (!eq) {
            /* No '=' — treat as a boolean flag (value length 0) */
            TXTRecordSetValue(out, entry, 0, NULL);
            continue;
        }

        char key[256];
        size_t klen = (size_t)(eq - entry);
        if (klen == 0 || klen >= sizeof(key))
            continue;
        memcpy(key, entry, klen);
        key[klen] = '\0';

        const char *val    = eq + 1;
        size_t      vlen   = strlen(val);
        if (vlen > MDNS_MAX_TXT_ENTRY_LEN)
            vlen = MDNS_MAX_TXT_ENTRY_LEN;

        TXTRecordSetValue(out, key, (uint8_t)vlen, val);
    }
}

/*
 * parse_txt_record — convert a raw TXT record blob into a heap-allocated
 * MdnsTxtArray.  Returns an empty array on error.
 */
static MdnsTxtArray parse_txt_record(uint16_t txt_len, const unsigned char *txt_ptr)
{
    MdnsTxtArray arr;
    arr.data = NULL;
    arr.len  = 0;

    if (!txt_ptr || txt_len == 0)
        return arr;

    uint16_t count = TXTRecordGetCount(txt_len, txt_ptr);
    if (count == 0)
        return arr;

    arr.data = (const char **)calloc(count, sizeof(const char *));
    if (!arr.data)
        return arr;

    for (uint16_t i = 0; i < count; i++) {
        char   key[256];
        uint8_t val_len = 0;
        const void *val_ptr = NULL;

        DNSServiceErrorType err = TXTRecordGetItemAtIndex(
            txt_len, txt_ptr, i,
            (uint16_t)sizeof(key), key,
            &val_len, &val_ptr);

        if (err != kDNSServiceErr_NoError)
            continue;

        /* Build "key=value" string */
        size_t klen  = strlen(key);
        size_t total = klen + 1 /* '=' */ + val_len + 1 /* '\0' */;
        char  *entry = (char *)malloc(total);
        if (!entry)
            continue;

        memcpy(entry, key, klen);
        entry[klen] = '=';
        if (val_ptr && val_len > 0)
            memcpy(entry + klen + 1, val_ptr, val_len);
        entry[klen + 1 + val_len] = '\0';

        arr.data[arr.len++] = entry;
    }

    return arr;
}

static void txt_array_free(MdnsTxtArray *arr)
{
    if (!arr || !arr->data)
        return;
    for (uint64_t i = 0; i < arr->len; i++)
        free((void *)arr->data[i]);
    free((void *)arr->data);
    arr->data = NULL;
    arr->len  = 0;
}

/* ---------------------------------------------------------------------- */
/* mdns_discovered_free                                                     */
/* ---------------------------------------------------------------------- */

void mdns_discovered_free(MdnsDiscovered *d)
{
    if (!d)
        return;
    free(d->name);
    free(d->host);
    txt_array_free(&d->txt);
    d->name = NULL;
    d->host = NULL;
}

/* ---------------------------------------------------------------------- */
/* mdns_is_available                                                        */
/* ---------------------------------------------------------------------- */

int mdns_is_available(void)
{
    return 1;
}

/* ---------------------------------------------------------------------- */
/* mdns_advertise                                                           */
/* ---------------------------------------------------------------------- */

int mdns_advertise(const MdnsServiceRecord *svc)
{
    if (!svc || !svc->name || !svc->type)
        return 0;

    pthread_mutex_lock(&g_mdns_mutex);
    state_init_locked();

    /* Check for duplicate name */
    for (int i = 0; i < MDNS_MAX_REGISTRATIONS; i++) {
        if (g_regs[i].active &&
            strcmp(g_regs[i].name, svc->name) == 0) {
            pthread_mutex_unlock(&g_mdns_mutex);
            return 0; /* already advertising this name */
        }
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MDNS_MAX_REGISTRATIONS; i++) {
        if (!g_regs[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_mdns_mutex);
        return 0; /* table full */
    }

    /* Build TXT record */
    TXTRecordRef txtrec;
    build_txt_record(&svc->txt, &txtrec);

    DNSServiceRef sdref = NULL;
    DNSServiceErrorType err = DNSServiceRegister(
        &sdref,
        0,                          /* flags */
        kDNSServiceInterfaceIndexAny,
        svc->name,
        svc->type,
        NULL,                       /* domain: default */
        NULL,                       /* host: default (this machine) */
        htons((uint16_t)svc->port),
        TXTRecordGetLength(&txtrec),
        TXTRecordGetBytesPtr(&txtrec),
        NULL,                       /* callback: fire-and-forget */
        NULL                        /* context */
    );

    TXTRecordDeallocate(&txtrec);

    if (err != kDNSServiceErr_NoError) {
        pthread_mutex_unlock(&g_mdns_mutex);
        return 0;
    }

    g_regs[slot].active = 1;
    strncpy(g_regs[slot].name, svc->name, sizeof(g_regs[slot].name) - 1);
    g_regs[slot].name[sizeof(g_regs[slot].name) - 1] = '\0';
    g_regs[slot].sdref = sdref;

    pthread_mutex_unlock(&g_mdns_mutex);
    return 1;
}

/* ---------------------------------------------------------------------- */
/* mdns_stop_advertise                                                      */
/* ---------------------------------------------------------------------- */

int mdns_stop_advertise(const char *name)
{
    if (!name)
        return 0;

    pthread_mutex_lock(&g_mdns_mutex);
    state_init_locked();

    for (int i = 0; i < MDNS_MAX_REGISTRATIONS; i++) {
        if (g_regs[i].active &&
            strcmp(g_regs[i].name, name) == 0) {
            DNSServiceRefDeallocate(g_regs[i].sdref);
            g_regs[i].active = 0;
            g_regs[i].sdref  = NULL;
            g_regs[i].name[0] = '\0';
            pthread_mutex_unlock(&g_mdns_mutex);
            return 1;
        }
    }

    pthread_mutex_unlock(&g_mdns_mutex);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Browse internals                                                         */
/* ---------------------------------------------------------------------- */

/*
 * Context passed to the Bonjour browse callback and resolve callback.
 */
typedef struct {
    MdnsBrowseSession *session; /* owning session (for callback + user_data) */
} BrowseCtx;

/*
 * Resolve callback context — used by mdns_resolve and mdns_browse's
 * per-instance resolve phase.
 */
typedef struct {
    volatile int    done;
    char            host[512];
    int32_t         port;
    MdnsTxtArray    txt;
    int             success;
} ResolveResult;

static void DNSSD_API resolve_callback(
    DNSServiceRef        sdref,
    DNSServiceFlags      flags,
    uint32_t             interface_index,
    DNSServiceErrorType  err,
    const char          *fullname,
    const char          *hosttarget,
    uint16_t             port_net,
    uint16_t             txt_len,
    const unsigned char *txt_ptr,
    void                *context)
{
    (void)sdref; (void)flags; (void)interface_index; (void)fullname;

    ResolveResult *res = (ResolveResult *)context;

    if (err != kDNSServiceErr_NoError || !hosttarget) {
        res->success = 0;
        res->done    = 1;
        return;
    }

    strncpy(res->host, hosttarget, sizeof(res->host) - 1);
    res->host[sizeof(res->host) - 1] = '\0';
    res->port    = (int32_t)ntohs(port_net);
    res->txt     = parse_txt_record(txt_len, txt_ptr);
    res->success = 1;
    res->done    = 1;
}

/*
 * do_resolve_locked — synchronously resolve name/type.
 * Called with g_mdns_mutex held; releases and re-acquires it during the
 * blocking poll so other threads are not starved.
 */
static int do_resolve_locked(
    const char   *name,
    const char   *service_type,
    uint32_t      interface_index,
    ResolveResult *out)
{
    memset(out, 0, sizeof(*out));

    DNSServiceRef sdref = NULL;
    DNSServiceErrorType err = DNSServiceResolve(
        &sdref,
        0,
        interface_index,
        name,
        service_type,
        NULL,          /* domain */
        resolve_callback,
        out
    );

    if (err != kDNSServiceErr_NoError)
        return 0;

    int fd = DNSServiceRefSockFD(sdref);

    /* Poll until resolved or timeout, releasing the lock while we wait */
    pthread_mutex_unlock(&g_mdns_mutex);

    struct timeval deadline;
    gettimeofday(&deadline, NULL);
    deadline.tv_sec  += MDNS_RESOLVE_TIMEOUT_MS / 1000;
    deadline.tv_usec += (MDNS_RESOLVE_TIMEOUT_MS % 1000) * 1000;
    if (deadline.tv_usec >= 1000000) {
        deadline.tv_sec  += 1;
        deadline.tv_usec -= 1000000;
    }

    while (!out->done) {
        struct timeval now;
        gettimeofday(&now, NULL);

        long remaining_us = (deadline.tv_sec  - now.tv_sec)  * 1000000L
                          + (deadline.tv_usec - now.tv_usec);
        if (remaining_us <= 0)
            break;

        struct timeval tv;
        tv.tv_sec  = remaining_us / 1000000L;
        tv.tv_usec = remaining_us % 1000000L;

        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(fd, &rset);

        int sel = select(fd + 1, &rset, NULL, NULL, &tv);
        if (sel > 0)
            DNSServiceProcessResult(sdref);
        else
            break; /* timeout or error */
    }

    pthread_mutex_lock(&g_mdns_mutex);

    DNSServiceRefDeallocate(sdref);
    return out->done && out->success;
}

/* Browse callback — Bonjour calls this from DNSServiceProcessResult */
static void DNSSD_API browse_callback(
    DNSServiceRef        sdref,
    DNSServiceFlags      flags,
    uint32_t             interface_index,
    DNSServiceErrorType  err,
    const char          *service_name,
    const char          *regtype,
    const char          *reply_domain,
    void                *context)
{
    (void)sdref; (void)reply_domain;

    BrowseCtx *ctx = (BrowseCtx *)context;
    if (!ctx || !ctx->session)
        return;

    if (err != kDNSServiceErr_NoError)
        return;

    /* Only report additions, not removals */
    if (!(flags & kDNSServiceFlagsAdd))
        return;

    /* Resolve the found instance to get host/port/txt */
    pthread_mutex_lock(&g_mdns_mutex);

    ResolveResult rr;
    int ok = do_resolve_locked(service_name, regtype, interface_index, &rr);

    if (ok) {
        MdnsDiscovered disc;
        disc.name = strdup(service_name);
        disc.host = strdup(rr.host);
        disc.port = rr.port;
        disc.txt  = rr.txt; /* ownership transferred */

        MdnsBrowseCallback cb        = ctx->session->cb;
        void              *user_data = ctx->session->user_data;

        pthread_mutex_unlock(&g_mdns_mutex);

        if (cb)
            cb(&disc, user_data);

        /* Free after callback returns */
        mdns_discovered_free(&disc);
    } else {
        txt_array_free(&rr.txt);
        pthread_mutex_unlock(&g_mdns_mutex);
    }
}

/* Background thread entry point for each browse session */
static void *browse_thread(void *arg)
{
    MdnsBrowseSession *session = (MdnsBrowseSession *)arg;

    int fd = DNSServiceRefSockFD(session->sdref);

    while (!session->stop_flag) {
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 200000; /* 200 ms poll interval */

        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(fd, &rset);

        int sel = select(fd + 1, &rset, NULL, NULL, &tv);
        if (sel > 0) {
            DNSServiceErrorType err = DNSServiceProcessResult(session->sdref);
            if (err != kDNSServiceErr_NoError)
                break;
        }
    }

    return NULL;
}

/* ---------------------------------------------------------------------- */
/* mdns_browse                                                              */
/* ---------------------------------------------------------------------- */

int mdns_browse(const char *service_type, MdnsBrowseCallback cb, void *user_data)
{
    if (!service_type || !cb)
        return 0;

    pthread_mutex_lock(&g_mdns_mutex);
    state_init_locked();

    /* Check for duplicate browse */
    for (int i = 0; i < MDNS_MAX_BROWSES; i++) {
        if (g_browses[i].active &&
            strcmp(g_browses[i].service_type, service_type) == 0) {
            pthread_mutex_unlock(&g_mdns_mutex);
            return 0;
        }
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MDNS_MAX_BROWSES; i++) {
        if (!g_browses[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_mdns_mutex);
        return 0;
    }

    MdnsBrowseSession *session = &g_browses[slot];
    memset(session, 0, sizeof(*session));

    strncpy(session->service_type, service_type,
            sizeof(session->service_type) - 1);
    session->service_type[sizeof(session->service_type) - 1] = '\0';
    session->cb        = cb;
    session->user_data = user_data;
    session->stop_flag = 0;

    /* Allocate context — freed when session is stopped */
    BrowseCtx *ctx = (BrowseCtx *)malloc(sizeof(BrowseCtx));
    if (!ctx) {
        pthread_mutex_unlock(&g_mdns_mutex);
        return 0;
    }
    ctx->session = session;

    DNSServiceRef sdref = NULL;
    DNSServiceErrorType err = DNSServiceBrowse(
        &sdref,
        0,
        kDNSServiceInterfaceIndexAny,
        service_type,
        NULL,           /* domain: default */
        browse_callback,
        ctx
    );

    if (err != kDNSServiceErr_NoError) {
        free(ctx);
        pthread_mutex_unlock(&g_mdns_mutex);
        return 0;
    }

    session->sdref  = sdref;
    session->active = 1;

    /* Stash ctx pointer in user_data field of sdref for cleanup.
     * We rely on ctx being freed in mdns_stop_browse. */

    pthread_t tid;
    if (pthread_create(&tid, NULL, browse_thread, session) != 0) {
        DNSServiceRefDeallocate(sdref);
        free(ctx);
        memset(session, 0, sizeof(*session));
        pthread_mutex_unlock(&g_mdns_mutex);
        return 0;
    }

    session->thread = tid;

    pthread_mutex_unlock(&g_mdns_mutex);
    return 1;
}

/* ---------------------------------------------------------------------- */
/* mdns_stop_browse                                                         */
/* ---------------------------------------------------------------------- */

int mdns_stop_browse(const char *service_type)
{
    if (!service_type)
        return 0;

    pthread_mutex_lock(&g_mdns_mutex);
    state_init_locked();

    MdnsBrowseSession *session = NULL;
    for (int i = 0; i < MDNS_MAX_BROWSES; i++) {
        if (g_browses[i].active &&
            strcmp(g_browses[i].service_type, service_type) == 0) {
            session = &g_browses[i];
            break;
        }
    }

    if (!session) {
        pthread_mutex_unlock(&g_mdns_mutex);
        return 0;
    }

    /* Signal thread to stop and wait for it */
    session->stop_flag = 1;
    pthread_t tid = session->thread;

    pthread_mutex_unlock(&g_mdns_mutex);
    pthread_join(tid, NULL);
    pthread_mutex_lock(&g_mdns_mutex);

    DNSServiceRefDeallocate(session->sdref);
    memset(session, 0, sizeof(*session));

    pthread_mutex_unlock(&g_mdns_mutex);
    return 1;
}

/* ---------------------------------------------------------------------- */
/* mdns_resolve                                                             */
/* ---------------------------------------------------------------------- */

MdnsOptDiscovered mdns_resolve(const char *name, const char *service_type)
{
    MdnsOptDiscovered result;
    memset(&result, 0, sizeof(result));

    if (!name || !service_type)
        return result;

    pthread_mutex_lock(&g_mdns_mutex);
    state_init_locked();

    ResolveResult rr;
    int ok = do_resolve_locked(name, service_type,
                               kDNSServiceInterfaceIndexAny, &rr);

    if (ok) {
        result.has_value      = 1;
        result.value.name     = strdup(name);
        result.value.host     = strdup(rr.host);
        result.value.port     = rr.port;
        result.value.txt      = rr.txt; /* ownership transferred */
    } else {
        txt_array_free(&rr.txt);
    }

    pthread_mutex_unlock(&g_mdns_mutex);
    return result;
}

/* =========================================================================
 * Linux stub
 * ========================================================================= */

#elif defined(__linux__)

void mdns_discovered_free(MdnsDiscovered *d)
{
    (void)d;
}

int mdns_is_available(void)     { return 0; }
int mdns_advertise(const MdnsServiceRecord *svc)   { (void)svc;  return 0; }
int mdns_stop_advertise(const char *name)          { (void)name; return 0; }

int mdns_browse(const char *service_type, MdnsBrowseCallback cb, void *user_data)
{
    (void)service_type; (void)cb; (void)user_data;
    return 0;
}

int mdns_stop_browse(const char *service_type)
{
    (void)service_type;
    return 0;
}

MdnsOptDiscovered mdns_resolve(const char *name, const char *service_type)
{
    MdnsOptDiscovered r;
    (void)name; (void)service_type;
    memset(&r, 0, sizeof(r));
    return r;
}

/* =========================================================================
 * Windows stub
 * ========================================================================= */

#elif defined(_WIN32) || defined(_WIN64)

void mdns_discovered_free(MdnsDiscovered *d)
{
    (void)d;
}

int mdns_is_available(void)     { return 0; }
int mdns_advertise(const MdnsServiceRecord *svc)   { (void)svc;  return 0; }
int mdns_stop_advertise(const char *name)          { (void)name; return 0; }

int mdns_browse(const char *service_type, MdnsBrowseCallback cb, void *user_data)
{
    (void)service_type; (void)cb; (void)user_data;
    return 0;
}

int mdns_stop_browse(const char *service_type)
{
    (void)service_type;
    return 0;
}

MdnsOptDiscovered mdns_resolve(const char *name, const char *service_type)
{
    MdnsOptDiscovered r;
    (void)name; (void)service_type;
    memset(&r, 0, sizeof(r));
    return r;
}

/* =========================================================================
 * Unknown platform — safe no-ops
 * ========================================================================= */

#else

void mdns_discovered_free(MdnsDiscovered *d)
{
    (void)d;
}

int mdns_is_available(void)     { return 0; }
int mdns_advertise(const MdnsServiceRecord *svc)   { (void)svc;  return 0; }
int mdns_stop_advertise(const char *name)          { (void)name; return 0; }

int mdns_browse(const char *service_type, MdnsBrowseCallback cb, void *user_data)
{
    (void)service_type; (void)cb; (void)user_data;
    return 0;
}

int mdns_stop_browse(const char *service_type)
{
    (void)service_type;
    return 0;
}

MdnsOptDiscovered mdns_resolve(const char *name, const char *service_type)
{
    MdnsOptDiscovered r;
    (void)name; (void)service_type;
    memset(&r, 0, sizeof(r));
    return r;
}

#endif /* __APPLE__ / __linux__ / _WIN32 / else */
