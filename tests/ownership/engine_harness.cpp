/*
 * Synthetic 4D engine boundary for ownership regressions.
 *
 * Compiles the real plugin entry point (4DPlugin-Simple-PostgreSQL-Client.cpp,
 * included below) and links the real support/4DPlugin-JSON.cpp, every class
 * of the bundled 4D Plugin API and the unmodified 4DPluginAPI.c. Only gCall4D
 * is replaced, and libpq is stubbed with synthetic results (no server is ever
 * contacted). Every engine resource (Unistrings, pictures, objects/
 * collections, handles) is a real heap block tracked in a live set, so leaks
 * are detected exactly and a dispose of something the engine no longer owns
 * (double free) is reported by name instead of being freed a second time.
 *
 * Engine semantics modelled (per the SDK contract):
 *  - PA_CreateVariable(eVK_Unistring/eVK_Picture) allocates an empty value.
 *  - PA_GetStringVariable returns a view borrowed from the variable.
 *  - PA_SetObjectProperty/PA_SetCollectionElement deep-copy strings and
 *    pictures and retain objects/collections; the caller still owns its
 *    variable.
 *  - PA_GetObjectProperty/PA_GetCollectionElement return an owned copy (or a
 *    retained object/collection) that must be cleared; on error the result
 *    is left uninitialised (poisoned here) and must be neither read nor
 *    cleared.
 *  - PA_ExecuteCommandByID returns an owned result (New Object/Collection).
 */

/* Resolved through -I <source-root>, so the same harness tests baseline and fixed trees. */
#include "4DPlugin-Simple-PostgreSQL-Client.cpp"

#include "PrivateTypes.h"
#include "EntryPoints.h"

#include <map>
#include <set>
#include <vector>

extern "C" Call4DProcPtr gCall4D;

namespace {

typedef std::basic_string<PA_Unichar> u16;

struct Obj {
    int rc;
    bool coll;
    std::map<u16, PA_Variable> props;
    std::vector<PA_Variable> items;
};

struct Handle { char *data; size_t size; };

std::set<void *> g_strings;
std::set<void *> g_pictures;
std::set<Obj *> g_objects;
std::set<void *> g_handles;
std::vector<std::string> g_violations;

/* PA_GetObjectProperty for this key fails: eb.fError is set and the result
   is left uninitialised. The poison is readable (tests fail by assertion,
   not by crashing) but never engine-owned, so clearing it is counted. */
std::string g_fail_key;
/* PA_GetCollectionElement at this index fails the same way. */
long g_fail_index = -2;
const char kPoisonMark = 0x5A;
int g_poison_clears = 0;
PA_Unichar kPoisonBuf[] = { 'B', 'A', 'D', '!', 0 };
PA_Unichar *const kPoison = kPoisonBuf;

PA_Variable undefined_var();

void poison(PA_Variable *out) {
    *out = undefined_var();
    out->fFiller = kPoisonMark;
    out->fType = eVK_Unistring;
    out->uValue.fString.fString = kPoison;
    out->uValue.fString.fLength = 4;
}

void violation(const std::string &what) {
    fprintf(stderr, "  ENGINE CONTRACT: %s\n", what.c_str());
    g_violations.push_back(what);
}

[[noreturn]] void die(const char *msg) {
    fprintf(stderr, "HARNESS ERROR: %s\n", msg);
    fflush(stderr);
    abort();
}

std::string narrow(const u16 &s) { std::string r; for(PA_Unichar c : s) r.push_back((char)c); return r; }
u16 widen(const std::string &s) { u16 r; for(char c : s) r.push_back((PA_Unichar)(unsigned char)c); return r; }

PA_Unistring new_string(const PA_Unichar *src, size_t len) {
    PA_Unichar *buf = (PA_Unichar *)malloc((len + 1) * sizeof(PA_Unichar));
    if(len) memcpy(buf, src, len * sizeof(PA_Unichar));
    buf[len] = 0;
    g_strings.insert(buf);
    PA_Unistring s;
    s.fLength = (PA_long32)len;
    s.fString = buf;
    s.fReserved1 = 0;
    s.fReserved2 = 0;
    return s;
}

void free_string(PA_Unistring *s) {
    if(!s->fString) return;
    if(s->fString == kPoison) {
        g_poison_clears++;
        fprintf(stderr, "  cleared the uninitialised result of a failed engine getter\n");
    } else if(!g_strings.erase(s->fString)) {
        violation("double_dispose_string");
    } else {
        free(s->fString);
    }
    s->fString = 0;
    s->fLength = 0;
}

void *new_picture() {
    void *p = malloc(16);
    g_pictures.insert(p);
    return p;
}

void free_picture(void *p) {
    if(!p) return;
    if(!g_pictures.erase(p)) { violation("double_dispose_picture"); return; }
    free(p);
}

Handle *new_handle(size_t size) {
    Handle *h = (Handle *)malloc(sizeof(Handle));
    h->size = size;
    h->data = (char *)malloc(size ? size : 1);
    g_handles.insert(h);
    return h;
}

void free_handle(Handle *h) {
    if(!h) return;
    if(!g_handles.erase(h)) { violation("double_dispose_handle"); return; }
    free(h->data);
    free(h);
}

Handle *live_handle(void *p) {
    if(!g_handles.count(p)) die("used an unknown handle");
    return (Handle *)p;
}

void release_object(Obj *o);

void clear_var(PA_Variable *v) {
    if(v->fFiller == kPoisonMark) {
        g_poison_clears++;
        fprintf(stderr, "  cleared the uninitialised result of a failed engine getter\n");
        v->fType = eVK_Undefined;
        v->fFiller = 0;
        return;
    }
    switch(v->fType) {
        case eVK_Unistring: free_string(&v->uValue.fString); break;
        case eVK_Picture: free_picture(v->uValue.fPicture); break;
        case eVK_Blob: free_handle((Handle *)v->uValue.fBlob.fHandle); break;
        case eVK_Object: if(v->uValue.fObject) release_object((Obj *)v->uValue.fObject); break;
        case eVK_Collection: if(v->uValue.fCollection) release_object((Obj *)v->uValue.fCollection); break;
        default: break;
    }
    v->fType = eVK_Undefined;
}

Obj *live(void *p) {
    if(!g_objects.count((Obj *)p)) die("used a dead object/collection");
    return (Obj *)p;
}

PA_Variable undefined_var() { PA_Variable v; memset(&v, 0, sizeof(v)); v.fType = eVK_Undefined; return v; }

PA_Variable copy_var(const PA_Variable &src) {
    PA_Variable dst = src;
    switch(src.fType) {
        case eVK_Unistring:
            dst.uValue.fString = new_string(src.uValue.fString.fString, src.uValue.fString.fLength);
            break;
        case eVK_Picture:
            if(!g_pictures.count(PA_Picture(src.uValue.fPicture))) die("stored a dead picture");
            dst.uValue.fPicture = new_picture();
            break;
        case eVK_Object:
            if(src.uValue.fObject) live(src.uValue.fObject)->rc++;
            break;
        case eVK_Collection:
            if(src.uValue.fCollection) live(src.uValue.fCollection)->rc++;
            break;
        default: break;
    }
    return dst;
}

Obj *new_object(bool coll = false) {
    Obj *o = new Obj();
    o->rc = 1;
    o->coll = coll;
    g_objects.insert(o);
    return o;
}

void release_object(Obj *o) {
    if(!g_objects.count(o)) { violation("double_release_object"); return; }
    if(--o->rc == 0) {
        for(auto &kv : o->props) clear_var(&kv.second);
        for(auto &v : o->items) clear_var(&v);
        g_objects.erase(o);
        delete o;
    }
}

u16 key_of(const PA_Unistring &k) { return u16(k.fString, k.fLength); }

PA_Variable object_result(Obj *o) {
    PA_Variable v = undefined_var();
    v.fType = o->coll ? eVK_Collection : eVK_Object;
    if(o->coll) v.uValue.fCollection = (PA_CollectionRef)o; else v.uValue.fObject = (PA_ObjectRef)o;
    return v;
}

void execute_command(EngineBlock *eb) {
    PA_Variable **params = (PA_Variable **)eb->fPtr1;
    PA_Variable *ret = (PA_Variable *)eb->fPtr2;
    switch(eb->fParam1) {
        case 1230: { /* OB Get type */
            Obj *o = live(params[0]->uValue.fObject);
            auto it = o->props.find(key_of(params[1]->uValue.fString));
            PA_long32 kind = it == o->props.end() ? eVK_Undefined : it->second.fType;
            if(kind == eVK_Unistring) kind = eVK_Text;
            ret->fType = eVK_Longint;
            ret->uValue.fLongint = kind;
            break;
        }
        case 1231: { /* OB Is defined */
            Obj *o = live(params[0]->uValue.fObject);
            ret->fType = eVK_Boolean;
            ret->uValue.fBoolean = o->props.count(key_of(params[1]->uValue.fString)) ? 1 : 0;
            break;
        }
        case 1471: *ret = object_result(new_object(false)); break; /* New Object */
        case 1472: *ret = object_result(new_object(true)); break; /* New Collection */
        default: {
            char msg[64];
            snprintf(msg, sizeof(msg), "unexpected PA_ExecuteCommandByID %d", (int)eb->fParam1);
            die(msg);
        }
    }
}

void engine(short selector, EngineBlock *eb) {
    switch(selector) {
        case EX_CREATE_UNISTRING: {
            const PA_Unichar *src = (const PA_Unichar *)eb->fHandle;
            size_t len = 0;
            while(src[len]) len++;
            eb->fUniString1 = new_string(src, len);
            break;
        }
        case EX_DISPOSE_UNISTRING:
            free_string(&eb->fUniString1);
            break;
        case EX_VARIABLE_TO_STRING: {
            PA_Variable *v = (PA_Variable *)eb->fHandle;
            if(v->fType == eVK_Unistring) eb->fUniString1 = v->uValue.fString; /* borrowed */
            break;
        }
        case EX_CREATE_PICTURE:
            eb->fPicture = new_picture();
            break;
        case EX_DISPOSE_PICTURE:
            free_picture(eb->fPicture);
            break;
        case EX_CLEAR_VARIABLE:
            clear_var((PA_Variable *)eb->fPtr1);
            break;
        case EX_YIELD_ABSOLUTE:
            break;
        case EX_GET_INFORMATION:
            eb->fParam2 = 0x00002000; /* 4D v20: PA_CreateObject uses New Object */
            break;
        case EX_SET_OBJ_VALUE: {
            Obj *o = live(eb->fPtr1);
            PA_Variable value = copy_var(*(PA_Variable *)eb->fPtr2);
            if(eb->fShort == eVK_Collection) {
                if(!o->coll || eb->fLongint < 0) die("bad collection set");
                size_t i = (size_t)eb->fLongint;
                if(i >= o->items.size()) o->items.resize(i + 1, undefined_var());
                clear_var(&o->items[i]);
                o->items[i] = value;
            } else {
                u16 k = key_of(eb->fUniString1);
                auto it = o->props.find(k);
                if(it != o->props.end()) clear_var(&it->second);
                o->props[k] = value;
            }
            break;
        }
        case EX_GET_OBJ_VALUE: {
            if(!eb->fPtr1) { violation("property_of_null_object"); *(PA_Variable *)eb->fPtr2 = undefined_var(); eb->fError = -1; break; }
            Obj *o = live(eb->fPtr1);
            PA_Variable *out = (PA_Variable *)eb->fPtr2;
            if(eb->fShort == eVK_Collection) {
                if(!o->coll) die("collection get on an object");
                *out = undefined_var();
                if(eb->fLongint == -1) {
                    out->fType = eVK_Longint;
                    out->uValue.fLongint = (PA_long32)o->items.size();
                } else if(eb->fLongint == g_fail_index) {
                    poison(out);
                    eb->fError = -1;
                } else if(eb->fLongint >= 0 && (size_t)eb->fLongint < o->items.size()) {
                    *out = copy_var(o->items[(size_t)eb->fLongint]);
                }
                break;
            }
            u16 k = key_of(eb->fUniString1);
            if(!g_fail_key.empty() && narrow(k) == g_fail_key) {
                poison(out);
                eb->fError = -1;
                break;
            }
            auto it = o->props.find(k);
            *out = it == o->props.end() ? undefined_var() : copy_var(it->second);
            break;
        }
        case EX_EXECUTE_COMMAND_BY_ID:
            execute_command(eb);
            break;
        case EX_HANDLE_MANAGER: {
            switch(eb->fParam1) {
                case 1: eb->fHandle = (PA_Handle)new_handle((size_t)eb->fParam2); break;
                case 2: free_handle((Handle *)eb->fHandle); break;
                case 5: eb->fParam3 = (sLONG_PTR)live_handle(eb->fHandle)->data; break;
                case 6: break;
                default: die("unexpected handle manager op");
            }
            break;
        }
        default: {
            char msg[64];
            snprintf(msg, sizeof(msg), "unmodelled engine selector %d", selector);
            die(msg);
        }
    }
}

/* ---------- libpq stub: synthetic connection and result ---------- */

bool g_conn_ok = true;
ExecStatusType g_status = PGRES_TUPLES_OK;
std::vector<std::string> g_fields;
std::vector<int> g_formats;
struct Cell { bool null; std::string value; };
std::vector<std::vector<Cell>> g_rows;

int g_open_conns = 0, g_open_results = 0, g_exec_calls = 0;
std::vector<std::string> g_sent;
std::string g_command;

char kEmpty[] = "";
char kDb[] = "synthetic_db";
char kUser[] = "synthetic_user";
char kHost[] = "localhost";
char kPort[] = "5432";
char kOk[] = "";
char kFail[] = "synthetic connection failure\n";

} // namespace

struct pg_conn { int magic; };
struct pg_result { ExecStatusType status; };

extern "C" {
PGconn *PQconnectdb(const char *) { g_open_conns++; return new pg_conn{42}; }
void PQfinish(PGconn *c) { if(c) { g_open_conns--; delete c; } }
char *PQdb(const PGconn *) { return kDb; }
char *PQuser(const PGconn *) { return kUser; }
char *PQpass(const PGconn *) { return kEmpty; }
char *PQhost(const PGconn *) { return kHost; }
char *PQport(const PGconn *) { return kPort; }
char *PQtty(const PGconn *) { return kEmpty; }
char *PQoptions(const PGconn *) { return kEmpty; }
ConnStatusType PQstatus(const PGconn *) { return g_conn_ok ? CONNECTION_OK : CONNECTION_BAD; }
PGTransactionStatusType PQtransactionStatus(const PGconn *) { return PQTRANS_IDLE; }
int PQprotocolVersion(const PGconn *) { return 3; }
char *PQerrorMessage(const PGconn *) { return g_conn_ok ? kOk : kFail; }
int PQsocket(const PGconn *) { return g_conn_ok ? 7 : -1; }
int PQbackendPID(const PGconn *) { return 1234; }
int PQconnectionNeedsPassword(const PGconn *) { return 0; }
int PQconnectionUsedPassword(const PGconn *) { return 0; }
int PQclientEncoding(const PGconn *) { return 6; }
int PQsslInUse(PGconn *) { return 0; }
PGresult *PQexecParams(PGconn *, const char *command, int nParams, const Oid *, const char *const *values,
                       const int *lengths, const int *, int) {
    g_exec_calls++;
    g_command = command;
    g_sent.clear();
    for(int i = 0; i < nParams; i++) g_sent.push_back(std::string(values[i], (size_t)lengths[i]));
    g_open_results++;
    return new pg_result{g_status};
}
ExecStatusType PQresultStatus(const PGresult *r) { return r ? r->status : PGRES_FATAL_ERROR; }
void PQclear(PGresult *r) { if(r) { g_open_results--; delete r; } }
int PQntuples(const PGresult *) { return (int)g_rows.size(); }
int PQnfields(const PGresult *) { return (int)g_fields.size(); }
char *PQfname(const PGresult *, int c) { return (char *)g_fields[(size_t)c].c_str(); }
int PQfformat(const PGresult *, int c) { return g_formats[(size_t)c]; }
char *PQgetvalue(const PGresult *, int r, int c) { return (char *)g_rows[(size_t)r][(size_t)c].value.c_str(); }
int PQgetlength(const PGresult *, int r, int c) { return (int)g_rows[(size_t)r][(size_t)c].value.size(); }
int PQgetisnull(const PGresult *, int r, int c) { return g_rows[(size_t)r][(size_t)c].null ? 1 : 0; }
}

namespace {

/* ---------- test helpers ---------- */

PA_Unistring U(const char *ascii) {
    u16 s = widen(ascii);
    return new_string(s.c_str(), s.size());
}

void put(Obj *o, const char *key, PA_Variable v) {
    PA_Unistring k = U(key);
    PA_SetObjectProperty((PA_ObjectRef)o, &k, v);
    PA_DisposeUnistring(&k);
}

void put_text(Obj *o, const char *key, const char *value) {
    PA_Variable v = undefined_var();
    PA_Unistring u = U(value);
    PA_SetStringVariable(&v, &u);
    put(o, key, v);
    PA_ClearVariable(&v);
}

void put_real(Obj *o, const char *key, double d) {
    PA_Variable v = undefined_var();
    PA_SetRealVariable(&v, d);
    put(o, key, v);
}

Obj *put_collection(Obj *o, const char *key) {
    Obj *c = new_object(true);
    PA_Variable v = object_result(c);
    put(o, key, v);
    PA_ClearVariable(&v);
    return c;
}

void push(Obj *c, PA_Variable v) {
    PA_SetCollectionElement((PA_CollectionRef)c, (PA_long32)c->items.size(), v);
    PA_ClearVariable(&v);
}

void push_text(Obj *c, const char *s) {
    PA_Variable v = undefined_var();
    PA_Unistring u = U(s);
    PA_SetStringVariable(&v, &u);
    push(c, v);
}

bool check(bool ok, const char *what) {
    if(!ok) fprintf(stderr, "  check failed: %s\n", what);
    return ok;
}

bool no_leaks() {
    bool ok = true;
    for(void *p : g_strings) {
        fprintf(stderr, "  LEAK: engine string \"%s\"\n", narrow(u16((PA_Unichar *)p)).c_str());
        ok = false;
    }
    if(!g_pictures.empty()) { fprintf(stderr, "  LEAK: %zu engine picture(s)\n", g_pictures.size()); ok = false; }
    if(!g_objects.empty()) { fprintf(stderr, "  LEAK: %zu engine object(s)/collection(s)\n", g_objects.size()); ok = false; }
    if(!g_handles.empty()) { fprintf(stderr, "  LEAK: %zu engine handle(s)\n", g_handles.size()); ok = false; }
    if(g_poison_clears) { fprintf(stderr, "  %d failed result(s) cleared\n", g_poison_clears); ok = false; }
    if(!g_violations.empty()) ok = false;
    return ok;
}

size_t count_strings(const char *s) {
    size_t n = 0;
    for(void *p : g_strings) if(narrow(u16((PA_Unichar *)p)) == s) n++;
    return n;
}

size_t live_objects(bool coll) {
    size_t n = 0;
    for(Obj *o : g_objects) if(o->coll == coll) n++;
    return n;
}

const PA_Variable *prop(Obj *o, const char *key) {
    auto it = o->props.find(widen(key));
    return it == o->props.end() ? NULL : &it->second;
}

std::string text_prop(Obj *o, const char *key) {
    const PA_Variable *v = prop(o, key);
    if(!v || v->fType != eVK_Unistring) return "<missing>";
    return narrow(u16(v->uValue.fString.fString, v->uValue.fString.fLength));
}

/* Calls the real `PQ EXECUTE` command; returns the result object. */
Obj *call_pq_execute(Obj *params, PA_long32 format = 0) {
    PA_Unistring connect = U("host=localhost dbname=synthetic_db");
    PA_Unistring sql = U("SELECT synthetic");
    PA_CollectionRef p3 = (PA_CollectionRef)params;
    PA_long32 p4 = format;
    void *args[4] = { &connect, &sql, &p3, &p4 };
    PA_ObjectRef result = NULL;
    PluginBlock pb;
    pb.fParameters = (PA_Variable *)args;
    pb.fResult = &result;
    pb.fData = NULL;
    PluginMain(1, &pb);
    PA_DisposeUnistring(&connect);
    PA_DisposeUnistring(&sql);
    return (Obj *)result;
}

bool libpq_released() {
    return check(g_open_conns == 0 && g_open_results == 0, "PGconn and PGresult released");
}

void two_text_rows() {
    g_status = PGRES_TUPLES_OK;
    g_fields = { "id", "name" };
    g_formats = { 0, 0 };
    g_rows = { { {false, "1"}, {false, "alice"} }, { {false, "2"}, {false, "bob"} } };
}

Obj *values_of(Obj *result) {
    const PA_Variable *v = result ? prop(result, "values") : NULL;
    return v && v->fType == eVK_Collection ? live(v->uValue.fCollection) : NULL;
}

bool rows_ok(Obj *result) {
    Obj *values = values_of(result);
    if(!check(values && values->items.size() == 2, "values has two rows")) return false;
    Obj *r0 = live(values->items[0].uValue.fObject), *r1 = live(values->items[1].uValue.fObject);
    return check(text_prop(r0, "id") == "1" && text_prop(r0, "name") == "alice"
              && text_prop(r1, "id") == "2" && text_prop(r1, "name") == "bob", "row values");
}

/* ---------- tests ---------- */

/* LIVE: every ob_set_s/ob_set_a overwrote the empty string allocated by
   PA_CreateVariable(eVK_Unistring) (connection metadata, text cells). */
bool defect_cmd_connect_failure_empty_string_leak() {
    g_conn_ok = false;
    Obj *result = call_pq_execute(NULL);
    bool ok = check(result && text_prop(result, "errorMessage") == kFail, "errorMessage returned");
    ok &= check(result && text_prop(result, "db") == "synthetic_db", "db returned");
    ok &= check(g_exec_calls == 0, "no query on a failed connection");
    ok &= libpq_released();
    if(result) release_object(result);
    ok &= check(count_strings("") == 0, "no empty string left over from PA_CreateVariable(eVK_Unistring)");
    return no_leaks() && ok;
}

/* LIVE: resultRows was created for every query but only handed over for
   tuple results. */
bool defect_cmd_command_ok_result_rows_leak() {
    g_status = PGRES_COMMAND_OK;
    Obj *result = call_pq_execute(NULL);
    bool ok = check(result && !prop(result, "values"), "no values for a command");
    ok &= check(g_exec_calls == 1, "query executed");
    ok &= libpq_released();
    if(result) release_object(result);
    ok &= check(live_objects(true) == 0, "no collection left after releasing the result");
    return no_leaks() && ok;
}

bool defect_cmd_fatal_error_result_rows_leak() {
    g_status = PGRES_FATAL_ERROR;
    Obj *result = call_pq_execute(NULL);
    bool ok = check(result && !prop(result, "values"), "no values for a failed query");
    ok &= libpq_released();
    if(result) release_object(result);
    ok &= check(live_objects(true) == 0, "no collection left after releasing the result");
    return no_leaks() && ok;
}

/* LIVE: the variable holding each row object was never cleared after
   PA_SetCollectionElement (which retains/copies), so every row leaked. */
bool defect_cmd_tuples_row_object_leak() {
    two_text_rows();
    Obj *result = call_pq_execute(NULL);
    bool ok = rows_ok(result);
    ok &= libpq_released();
    if(result) release_object(result);
    ok &= check(live_objects(false) == 0, "no row object left after releasing the result");
    return no_leaks() && ok;
}

/* LIVE: binary cells go through C_BLOB/ob_set_a(CUTF16String)/ob_set_o. */
bool defect_cmd_tuples_binary_and_null_cells() {
    g_status = PGRES_TUPLES_OK;
    g_fields = { "bin", "nothing" };
    g_formats = { 1, 0 };
    g_rows = { { {false, std::string("\x00\x01\x02\xff", 4)}, {true, ""} } };
    Obj *result = call_pq_execute(NULL, 1);
    Obj *values = values_of(result);
    bool ok = check(values && values->items.size() == 1, "one row");
    if(ok) {
        Obj *row = live(values->items[0].uValue.fObject);
        const PA_Variable *bin = prop(row, "bin");
        ok &= check(bin && bin->fType == eVK_Object && text_prop(live(bin->uValue.fObject), "data") == "AAEC/w==", "base64 cell");
        const PA_Variable *n = prop(row, "nothing");
        ok &= check(n && (unsigned char)n->fType == eVK_Null, "null cell");
    }
    ok &= libpq_released();
    if(result) release_object(result);
    return no_leaks() && ok;
}

/* LIVE: each PA_GetCollectionElement copy (e.g. a text parameter) was never
   cleared. */
bool defect_cmd_param_elements_not_cleared() {
    two_text_rows();
    Obj *params = new_object(true);
    push_text(params, "abc");
    PA_Variable v = undefined_var();
    PA_SetRealVariable(&v, 1.5); push(params, v);
    PA_SetLongintVariable(&v, 7); push(params, v);
    PA_SetBooleanVariable(&v, 1); push(params, v);
    Obj *result = call_pq_execute(params);
    bool ok = check(g_sent == std::vector<std::string>({ "abc", "1.5", "7", "true" }), "parameters sent to libpq");
    ok &= check(count_strings("abc") == 1, "only the caller's own copy of \"abc\" remains");
    ok &= rows_ok(result);
    ok &= libpq_released();
    ok &= check(g_objects.count(params) && params->rc == 1, "caller's collection untouched");
    release_object(params);
    if(result) release_object(result);
    return no_leaks() && ok;
}

/* HARDENING: a failed PA_GetCollectionElement leaves its result
   uninitialised; it must be neither read nor cleared. */
bool hardening_cmd_param_element_error_untouched() {
    two_text_rows();
    Obj *params = new_object(true);
    push_text(params, "a");
    push_text(params, "b");
    g_fail_index = 1;
    Obj *result = call_pq_execute(params);
    g_fail_index = -2;
    bool ok = check(g_sent == std::vector<std::string>({ "a", "null" }), "failed element sent as null, not garbage");
    ok &= libpq_released();
    release_object(params);
    if(result) release_object(result);
    return no_leaks() && ok;
}

bool defect_dormant_ob_get_a_result_not_cleared() {
    Obj *o = new_object(false);
    put_text(o, "s", "text");
    CUTF8String s;
    bool r = ob_get_a((PA_ObjectRef)o, L"s", &s);
    release_object(o);
    bool ok = check(r && s == (const uint8_t *)"text", "value read");
    return no_leaks() && ok;
}

bool defect_dormant_ob_get_n_wrong_kind_not_cleared() {
    Obj *o = new_object(false);
    put_text(o, "n", "high");
    double d = ob_get_n((PA_ObjectRef)o, L"n");
    release_object(o);
    bool ok = check(d == 0.0, "wrong kind yields 0");
    return no_leaks() && ok;
}

bool defect_dormant_ob_get_b_wrong_kind_not_cleared() {
    Obj *o = new_object(false);
    put_text(o, "flag", "yes");
    bool b = ob_get_b((PA_ObjectRef)o, L"flag");
    release_object(o);
    bool ok = check(!b, "wrong kind yields false");
    return no_leaks() && ok;
}

/* DORMANT: the retained collection reference was never released, so the
   collection outlived its owner. */
bool defect_dormant_ob_get_c_reference_not_released() {
    Obj *o = new_object(false);
    Obj *c = put_collection(o, "items");
    PA_CollectionRef r = ob_get_c((PA_ObjectRef)o, L"items");
    bool ok = check(r == (PA_CollectionRef)c, "collection returned");
    release_object(o);
    return no_leaks() && ok;
}

/* DORMANT: ob_set_a(wchar_t*) and ob_set_p overwrote the value from
   PA_CreateVariable(eVK_Unistring/eVK_Picture). */
bool defect_dormant_ob_set_empty_owner_leaks() {
    Obj *o = new_object(false);
    ob_set_a((PA_ObjectRef)o, L"k1", L"v1");
    PA_Picture pic = PA_CreatePicture(NULL, 0);
    // ob_set_p consumes `pic` (shallow set, deep-copying set, then clear).
    ob_set_p((PA_ObjectRef)o, L"k2", pic);
    bool ok = check(o->props.size() == 2, "two properties set");
    release_object(o);
    return no_leaks() && ok;
}

/* HARDENING: on error PA_GetObjectProperty leaves its result uninitialised. */
bool hardening_ob_get_error_result_untouched() {
    Obj *o = new_object(false);
    put_text(o, "a", "x");
    put_real(o, "n", 1.0);
    g_fail_key = "a";
    CUTF8String s;
    ob_get_a((PA_ObjectRef)o, L"a", &s);
    g_fail_key = "n";
    double d = ob_get_n((PA_ObjectRef)o, L"n");
    g_fail_key.clear();
    release_object(o);
    bool ok = check(s.empty() && d == 0.0, "failed reads yield nothing");
    return no_leaks() && ok;
}

/* ---------- controls (pass on both trees) ---------- */

/* Behaviour preserved: libpq handles are always released and the command
   returns the same shape. */
bool control_cmd_libpq_released_and_shape() {
    two_text_rows();
    Obj *result = call_pq_execute(NULL);
    bool ok = rows_ok(result);
    ok &= check(result && text_prop(result, "user") == "synthetic_user", "metadata");
    ok &= check(g_sent.empty() && g_command == "SELECT synthetic", "query without parameters");
    ok &= libpq_released();
    if(result) release_object(result);
    return ok;
}

bool control_cmd_connect_failure_shape() {
    g_conn_ok = false;
    Obj *result = call_pq_execute(NULL);
    bool ok = check(result && text_prop(result, "errorMessage") == kFail && !prop(result, "values"), "failure reported");
    ok &= libpq_released();
    if(result) release_object(result);
    return ok;
}

bool control_ob_set_n_b_o_c_round_trip() {
    Obj *o = new_object(false);
    ob_set_n((PA_ObjectRef)o, L"n", 2.5);
    ob_set_b((PA_ObjectRef)o, L"b", true);
    ob_set_o((PA_ObjectRef)o, "o", PA_CreateObject());
    ob_set_c((PA_ObjectRef)o, L"c", PA_CreateCollection());
    bool ok = check(o->props.size() == 4, "four properties set");
    ok &= check(ob_is_defined((PA_ObjectRef)o, L"n") && !ob_is_defined((PA_ObjectRef)o, L"x"), "ob_is_defined");
    release_object(o);
    return no_leaks() && ok;
}

bool control_c_blob_base64() {
    C_BLOB data;
    data.setBytes((const uint8_t *)"\x00\x01\x02\xff", 4);
    C_TEXT t;
    data.toB64Text(&t);
    CUTF8String u8;
    t.copyUTF8String(&u8);
    return check(u8 == (const uint8_t *)"AAEC/w==", "base64") && no_leaks();
}

struct Test { const char *name; bool (*fn)(); };
const Test kTests[] = {
    { "defect_cmd_connect_failure_empty_string_leak", defect_cmd_connect_failure_empty_string_leak },
    { "defect_cmd_command_ok_result_rows_leak", defect_cmd_command_ok_result_rows_leak },
    { "defect_cmd_fatal_error_result_rows_leak", defect_cmd_fatal_error_result_rows_leak },
    { "defect_cmd_tuples_row_object_leak", defect_cmd_tuples_row_object_leak },
    { "defect_cmd_tuples_binary_and_null_cells", defect_cmd_tuples_binary_and_null_cells },
    { "defect_cmd_param_elements_not_cleared", defect_cmd_param_elements_not_cleared },
    { "hardening_cmd_param_element_error_untouched", hardening_cmd_param_element_error_untouched },
    { "defect_dormant_ob_get_a_result_not_cleared", defect_dormant_ob_get_a_result_not_cleared },
    { "defect_dormant_ob_get_n_wrong_kind_not_cleared", defect_dormant_ob_get_n_wrong_kind_not_cleared },
    { "defect_dormant_ob_get_b_wrong_kind_not_cleared", defect_dormant_ob_get_b_wrong_kind_not_cleared },
    { "defect_dormant_ob_get_c_reference_not_released", defect_dormant_ob_get_c_reference_not_released },
    { "defect_dormant_ob_set_empty_owner_leaks", defect_dormant_ob_set_empty_owner_leaks },
    { "hardening_ob_get_error_result_untouched", hardening_ob_get_error_result_untouched },
    { "control_cmd_libpq_released_and_shape", control_cmd_libpq_released_and_shape },
    { "control_cmd_connect_failure_shape", control_cmd_connect_failure_shape },
    { "control_ob_set_n_b_o_c_round_trip", control_ob_set_n_b_o_c_round_trip },
    { "control_c_blob_base64", control_c_blob_base64 },
};

} // namespace

int main(int argc, char **argv) {
    gCall4D = engine;
    if(argc == 2 && !strcmp(argv[1], "--list")) {
        for(const Test &t : kTests) printf("%s\n", t.name);
        return 0;
    }
    if(argc != 2) { fprintf(stderr, "usage: %s --list | <test>\n", argv[0]); return 2; }
    for(const Test &t : kTests) {
        if(!strcmp(argv[1], t.name)) {
            bool ok = t.fn();
            printf("%s: %s\n", t.name, ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
    }
    fprintf(stderr, "unknown test %s\n", argv[1]);
    return 2;
}
