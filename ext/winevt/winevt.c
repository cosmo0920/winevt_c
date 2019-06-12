#include <ruby.h>
#include <ruby/encoding.h>

#ifdef __GNUC__
# include <w32api.h>
# define MINIMUM_WINDOWS_VERSION WindowsVista
#else /* __GNUC__ */
# define MINIMUM_WINDOWS_VERSION 0x0600 /* Vista */
#endif /* __GNUC__ */

#ifdef _WIN32_WINNT
#  undef WIN32_WINNT
#endif /* WIN32_WINNT */
#define _WIN32_WINNT MINIMUM_WINDOWS_VERSION

#include <winevt.h>
#define EventQuery(object) ((struct WinevtQuery *)DATA_PTR(object))
#define EventBookMark(object) ((struct WinevtBookmark *)DATA_PTR(object))
#define EventChannel(object) ((struct WinevtChannel *)DATA_PTR(object))

VALUE rb_mWinevt;
VALUE rb_cEventLog;
VALUE rb_cSubscribe;
VALUE rb_cChannel;
VALUE rb_cQuery;
VALUE rb_cBookmark;
VALUE rb_eWinevtQueryError;

static ID id_call;
static void channel_free(void *ptr);
static void subscribe_free(void *ptr);
static void query_free(void *ptr);
static void bookmark_free(void *ptr);
static char* render_event(EVT_HANDLE handle, DWORD flags);
static char* wstr_to_mbstr(UINT cp, const WCHAR *wstr, int clen);
static VALUE render_values_event(EVT_HANDLE hEvent);
static VALUE render_system_event(EVT_HANDLE hEvent);

static const rb_data_type_t rb_winevt_channel_type = {
  "winevt/channel", {
    0, channel_free, 0,
  }, NULL, NULL,
  RUBY_TYPED_FREE_IMMEDIATELY
};

struct WinevtChannel {
  EVT_HANDLE channels;
};

static const rb_data_type_t rb_winevt_query_type = {
  "winevt/query", {
    0, query_free, 0,
  }, NULL, NULL,
  RUBY_TYPED_FREE_IMMEDIATELY
};

struct WinevtQuery {
  EVT_HANDLE query;
  EVT_HANDLE event;
  ULONG      count;
  LONG       offset;
  LONG       timeout;
  BOOL       doesRenderHash;
};

static const rb_data_type_t rb_winevt_bookmark_type = {
  "winevt/bookmark", {
    0, bookmark_free, 0,
  }, NULL, NULL,
  RUBY_TYPED_FREE_IMMEDIATELY
};

struct WinevtBookmark {
  EVT_HANDLE bookmark;
  ULONG      count;
};

static const rb_data_type_t rb_winevt_subscribe_type = {
  "winevt/subscribe", {
    0, subscribe_free, 0,
  }, NULL, NULL,
  RUBY_TYPED_FREE_IMMEDIATELY
};

struct WinevtSubscribe {
  HANDLE     signalEvent;
  EVT_HANDLE subscription;
  EVT_HANDLE bookmark;
  EVT_HANDLE event;
  DWORD      flags;
  BOOL       tailing;
  BOOL       doesRenderHash;
};

static void
channel_free(void *ptr)
{
  struct WinevtChannel *winevtChannel = (struct WinevtChannel *)ptr;
  if (winevtChannel->channels)
    EvtClose(winevtChannel->channels);

  xfree(ptr);
}

static VALUE
rb_winevt_channel_alloc(VALUE klass)
{
  VALUE obj;
  struct WinevtChannel *winevtChannel;
  obj = TypedData_Make_Struct(klass,
                              struct WinevtChannel,
                              &rb_winevt_channel_type,
                              winevtChannel);
  return obj;
}

static VALUE
rb_winevt_channel_initialize(VALUE klass)
{
  return Qnil;
}

static VALUE
rb_winevt_channel_each(VALUE self)
{
  EVT_HANDLE hChannels;
  struct WinevtChannel *winevtChannel;
  char *errBuf;
  char * result;
  LPWSTR buffer = NULL;
  LPWSTR temp = NULL;
  DWORD bufferSize = 0;
  DWORD bufferUsed = 0;
  DWORD status = ERROR_SUCCESS;

  RETURN_ENUMERATOR(self, 0, 0);

  TypedData_Get_Struct(self, struct WinevtChannel, &rb_winevt_channel_type, winevtChannel);

  hChannels = EvtOpenChannelEnum(NULL, 0);

  if (hChannels) {
    winevtChannel->channels = hChannels;
  } else {
    sprintf(errBuf, "Failed to enumerate channels with %s\n", GetLastError());
    rb_raise(rb_eRuntimeError, errBuf);
  }

  while (1) {
    if (!EvtNextChannelPath(winevtChannel->channels, bufferSize, buffer, &bufferUsed)) {
      status = GetLastError();

      if (ERROR_NO_MORE_ITEMS == status) {
        break;
      } else if (ERROR_INSUFFICIENT_BUFFER == status) {
        bufferSize = bufferUsed;
        temp = (LPWSTR)realloc(buffer, bufferSize * sizeof(WCHAR));
        if (temp) {
          buffer = temp;
          temp = NULL;
          EvtNextChannelPath(winevtChannel->channels, bufferSize, buffer, &bufferUsed);
        } else {
          status = ERROR_OUTOFMEMORY;
          rb_raise(rb_eRuntimeError, "realloc failed");
        }
      } else {
        sprintf(errBuf, "EvtNextChannelPath failed with %lu.\n", status);
        rb_raise(rb_eRuntimeError, errBuf);
      }
    }

    result = wstr_to_mbstr(CP_UTF8, buffer, -1);

    rb_yield(rb_str_new2(result));
  }

  return Qnil;
}

static char *
wstr_to_mbstr(UINT cp, const WCHAR *wstr, int clen)
{
    char *ptr;
    int len = WideCharToMultiByte(cp, 0, wstr, clen, NULL, 0, NULL, NULL);
    if (!(ptr = malloc(len))) return 0;
    WideCharToMultiByte(cp, 0, wstr, clen, ptr, len, NULL, NULL);

    return ptr;
}


static void
subscribe_free(void *ptr)
{
  struct WinevtSubscribe *winevtSubscribe = (struct WinevtSubscribe *)ptr;
  if (winevtSubscribe->signalEvent)
    CloseHandle(winevtSubscribe->signalEvent);

  if (winevtSubscribe->subscription)
    EvtClose(winevtSubscribe->subscription);

  if (winevtSubscribe->bookmark)
    EvtClose(winevtSubscribe->bookmark);

  if (winevtSubscribe->event)
    EvtClose(winevtSubscribe->event);

  xfree(ptr);
}

static VALUE
rb_winevt_subscribe_alloc(VALUE klass)
{
  VALUE obj;
  struct WinevtSubscribe *winevtSubscribe;
  obj = TypedData_Make_Struct(klass,
                              struct WinevtSubscribe,
                              &rb_winevt_subscribe_type,
                              winevtSubscribe);
  return obj;
}

static VALUE
rb_winevt_subscribe_initialize(VALUE self)
{
  return Qnil;
}

static VALUE
rb_winevt_subscribe_set_tail(VALUE self, VALUE rb_tailing_p)
{
  struct WinevtSubscribe *winevtSubscribe;

  TypedData_Get_Struct(self, struct WinevtSubscribe, &rb_winevt_subscribe_type, winevtSubscribe);

  winevtSubscribe->tailing = RTEST(rb_tailing_p);

  return Qnil;
}

static VALUE
rb_winevt_subscribe_tail_p(VALUE self, VALUE rb_flag)
{
  struct WinevtSubscribe *winevtSubscribe;

  TypedData_Get_Struct(self, struct WinevtSubscribe, &rb_winevt_subscribe_type, winevtSubscribe);

  return winevtSubscribe->tailing ? Qtrue : Qfalse;
}

static VALUE
rb_winevt_subscribe_set_does_render_hash(VALUE self, VALUE rb_render_hash_p)
{
  struct WinevtSubscribe *winevtSubscribe;

  TypedData_Get_Struct(self, struct WinevtSubscribe, &rb_winevt_subscribe_type, winevtSubscribe);

  winevtSubscribe->doesRenderHash = RTEST(rb_render_hash_p);

  return Qnil;
}

static VALUE
rb_winevt_subscribe_get_render_hash_p(VALUE self, VALUE rb_render_hash_p)
{
  struct WinevtSubscribe *winevtSubscribe;

  TypedData_Get_Struct(self, struct WinevtSubscribe, &rb_winevt_subscribe_type, winevtSubscribe);

  return winevtSubscribe->doesRenderHash ? Qtrue : Qfalse;
}

static VALUE
rb_winevt_subscribe_subscribe(int argc, VALUE argv, VALUE self)
{
  VALUE rb_path, rb_query, rb_bookmark;
  EVT_HANDLE hSubscription = NULL, hBookmark = NULL;
  HANDLE hSignalEvent;
  DWORD len, flags;
  VALUE wpathBuf, wqueryBuf;
  PWSTR path, query;
  DWORD status = ERROR_SUCCESS;
  struct WinevtBookmark *winevtBookmark;
  struct WinevtSubscribe *winevtSubscribe;

  hSignalEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

  TypedData_Get_Struct(self, struct WinevtSubscribe, &rb_winevt_subscribe_type, winevtSubscribe);

  rb_scan_args(argc, argv, "21", &rb_path, &rb_query, &rb_bookmark);
  Check_Type(rb_path, T_STRING);
  Check_Type(rb_query, T_STRING);

  if (rb_obj_is_kind_of(rb_bookmark, rb_cBookmark)) {
    hBookmark = EventBookMark(rb_bookmark)->bookmark;
  }

  // path : To wide char
  len = MultiByteToWideChar(CP_UTF8, 0, RSTRING_PTR(rb_path), RSTRING_LEN(rb_path), NULL, 0);
  path = ALLOCV_N(WCHAR, wpathBuf, len+1);
  MultiByteToWideChar(CP_UTF8, 0, RSTRING_PTR(rb_path), RSTRING_LEN(rb_path), path, len);
  path[len] = L'\0';

  // query : To wide char
  len = MultiByteToWideChar(CP_UTF8, 0, RSTRING_PTR(rb_query), RSTRING_LEN(rb_query), NULL, 0);
  query = ALLOCV_N(WCHAR, wqueryBuf, len+1);
  MultiByteToWideChar(CP_UTF8, 0, RSTRING_PTR(rb_query), RSTRING_LEN(rb_query), query, len);
  query[len] = L'\0';

  if (hBookmark){
    flags |= EvtSubscribeStartAfterBookmark;
  } else if (winevtSubscribe->tailing) {
    flags |= EvtSubscribeToFutureEvents;
  } else {
    flags |= EvtSubscribeStartAtOldestRecord;
  }

  hSubscription = EvtSubscribe(NULL, hSignalEvent, path, query, hBookmark, NULL, NULL, flags);

  winevtSubscribe->signalEvent = hSignalEvent;
  winevtSubscribe->subscription = hSubscription;
  if (hBookmark) {
    winevtSubscribe->bookmark = hBookmark;
  } else {
    winevtSubscribe->bookmark = EvtCreateBookmark(NULL);
  }

  status = GetLastError();

  if (status == ERROR_SUCCESS)
    return Qtrue;

  return Qfalse;
}

static VALUE
rb_winevt_subscribe_next(VALUE self)
{
  EVT_HANDLE event;
  ULONG      count;
  struct WinevtSubscribe *winevtSubscribe;

  TypedData_Get_Struct(self, struct WinevtSubscribe, &rb_winevt_subscribe_type, winevtSubscribe);

  if (EvtNext(winevtSubscribe->subscription, 1, &event, INFINITE, 0, &count) != FALSE) {
    winevtSubscribe->event = event;
    EvtUpdateBookmark(winevtSubscribe->bookmark, winevtSubscribe->event);

    return Qtrue;
  }

  return Qfalse;
}

static VALUE
rb_winevt_subscribe_render(VALUE self)
{
  char* result;
  struct WinevtSubscribe *winevtSubscribe;
  VALUE ary, hash;

  TypedData_Get_Struct(self, struct WinevtSubscribe, &rb_winevt_subscribe_type, winevtSubscribe);

  if (winevtSubscribe->doesRenderHash) {
    ary = render_values_event(winevtSubscribe->event);
    hash = render_system_event(winevtSubscribe->event);
    rb_hash_aset(hash, ID2SYM(rb_intern("Data")), ary);
    return hash;
  } else {
    result = render_event(winevtSubscribe->event, EvtRenderEventXml);
    return rb_str_new2(result);
  }
}

static VALUE
rb_winevt_subscribe_each(VALUE self)
{
  struct WinevtSubscribe *winevtSubscribe;

  RETURN_ENUMERATOR(self, 0, 0);

  TypedData_Get_Struct(self, struct WinevtSubscribe, &rb_winevt_subscribe_type, winevtSubscribe);

  while (rb_winevt_subscribe_next(self)) {
    rb_yield(rb_winevt_subscribe_render(self));
  }

  return Qnil;
}

static VALUE
rb_winevt_subscribe_get_bookmark(VALUE self)
{
  char* result;
  struct WinevtSubscribe *winevtSubscribe;

  TypedData_Get_Struct(self, struct WinevtSubscribe, &rb_winevt_subscribe_type, winevtSubscribe);

  result = render_event(winevtSubscribe->bookmark, EvtRenderBookmark);

  return rb_str_new2(result);
}

static void
bookmark_free(void *ptr)
{
  struct WinevtBookmark *winevtBookmark = (struct WinevtBookmark *)ptr;
  if (winevtBookmark->bookmark)
    EvtClose(winevtBookmark->bookmark);

  xfree(ptr);
}

static VALUE
rb_winevt_bookmark_alloc(VALUE klass)
{
  VALUE obj;
  struct WinevtBookmark *winevtBookmark;
  obj = TypedData_Make_Struct(klass,
                              struct WinevtBookmark,
                              &rb_winevt_bookmark_type,
                              winevtBookmark);
  return obj;
}

static VALUE
rb_winevt_bookmark_initialize(int argc, VALUE *argv, VALUE self)
{
  PWSTR bookmarkXml;
  VALUE wbookmarkXmlBuf;
  DWORD len;
  struct WinevtBookmark *winevtBookmark;

  TypedData_Get_Struct(self, struct WinevtBookmark, &rb_winevt_bookmark_type, winevtBookmark);

  if (argc == 0) {
    winevtBookmark->bookmark = EvtCreateBookmark(NULL);
  } else if (argc == 1) {
    VALUE rb_bookmarkXml;
    rb_scan_args(argc, argv, "10", &rb_bookmarkXml);
    Check_Type(rb_bookmarkXml, T_STRING);

    // bookmarkXml : To wide char
    len = MultiByteToWideChar(CP_UTF8, 0, RSTRING_PTR(rb_bookmarkXml), RSTRING_LEN(rb_bookmarkXml), NULL, 0);
    bookmarkXml = ALLOCV_N(WCHAR, wbookmarkXmlBuf, len+1);
    MultiByteToWideChar(CP_UTF8, 0, RSTRING_PTR(rb_bookmarkXml), RSTRING_LEN(rb_bookmarkXml), bookmarkXml, len);
    bookmarkXml[len] = L'\0';
    winevtBookmark->bookmark = EvtCreateBookmark(bookmarkXml);
    ALLOCV_END(wbookmarkXmlBuf);
  }

  return Qnil;
}

static VALUE
rb_winevt_bookmark_update(VALUE self, VALUE event)
{
  struct WinevtQuery *winevtQuery;
  struct WinevtBookmark *winevtBookmark;

  winevtQuery = EventQuery(event);

  TypedData_Get_Struct(self, struct WinevtBookmark, &rb_winevt_bookmark_type, winevtBookmark);

  if(EvtUpdateBookmark(winevtBookmark->bookmark, winevtQuery->event))
    return Qtrue;

  return Qfalse;
}

static VALUE
rb_winevt_bookmark_render(VALUE self)
{
  char* result;
  struct WinevtBookmark *winevtBookmark;

  TypedData_Get_Struct(self, struct WinevtBookmark, &rb_winevt_bookmark_type, winevtBookmark);

  result = render_event(winevtBookmark->bookmark, EvtRenderBookmark);

  return rb_str_new2(result);
}

static void
query_free(void *ptr)
{
  struct WinevtQuery *winevtQuery = (struct WinevtQuery *)ptr;
  if (winevtQuery->query)
    EvtClose(winevtQuery->query);

  if (winevtQuery->event)
    EvtClose(winevtQuery->event);

  xfree(ptr);
}

static VALUE
rb_winevt_query_alloc(VALUE klass)
{
  VALUE obj;
  struct WinevtQuery *winevtQuery;
  obj = TypedData_Make_Struct(klass,
                              struct WinevtQuery,
                              &rb_winevt_query_type,
                              winevtQuery);
  return obj;
}

static VALUE
rb_winevt_query_initialize(VALUE self, VALUE channel, VALUE xpath)
{
  PWSTR evtChannel, evtXPath;
  struct WinevtQuery *winevtQuery;
  DWORD len;
  VALUE wchannelBuf, wpathBuf;

  Check_Type(channel, T_STRING);
  Check_Type(xpath, T_STRING);

  // channel : To wide char
  len = MultiByteToWideChar(CP_UTF8, 0, RSTRING_PTR(channel), RSTRING_LEN(channel), NULL, 0);
  evtChannel = ALLOCV_N(WCHAR, wchannelBuf, len+1);
  MultiByteToWideChar(CP_UTF8, 0, RSTRING_PTR(channel), RSTRING_LEN(channel), evtChannel, len);
  evtChannel[len] = L'\0';

  // xpath : To wide char
  len = MultiByteToWideChar(CP_UTF8, 0, RSTRING_PTR(xpath), RSTRING_LEN(xpath), NULL, 0);
  evtXPath = ALLOCV_N(WCHAR, wpathBuf, len+1);
  MultiByteToWideChar(CP_UTF8, 0, RSTRING_PTR(xpath), RSTRING_LEN(xpath), evtXPath, len);
  evtXPath[len] = L'\0';

  TypedData_Get_Struct(self, struct WinevtQuery, &rb_winevt_query_type, winevtQuery);

  winevtQuery->query = EvtQuery(NULL, evtChannel, evtXPath,
                                EvtQueryChannelPath | EvtQueryTolerateQueryErrors);
  winevtQuery->offset = 0L;
  winevtQuery->timeout = 0L;
  winevtQuery->doesRenderHash = FALSE;

  ALLOCV_END(wchannelBuf);
  ALLOCV_END(wpathBuf);

  return Qnil;
}

static VALUE
rb_winevt_query_set_does_render_hash(VALUE self, VALUE rb_render_hash_p)
{
  struct WinevtQuery *winevtQuery;
  struct WinevtRenderContext *winevtRenderContext;

  TypedData_Get_Struct(self, struct WinevtQuery, &rb_winevt_query_type, winevtQuery);

  winevtQuery->doesRenderHash = RTEST(rb_render_hash_p);

  return Qnil;
}

static VALUE
rb_winevt_query_get_render_hash_p(VALUE self)
{
  struct WinevtQuery *winevtQuery;

  TypedData_Get_Struct(self, struct WinevtQuery, &rb_winevt_query_type, winevtQuery);

  return winevtQuery->doesRenderHash ? Qtrue : Qfalse;
}

static VALUE
rb_winevt_query_get_offset(VALUE self, VALUE offset)
{
  struct WinevtQuery *winevtQuery;

  TypedData_Get_Struct(self, struct WinevtQuery, &rb_winevt_query_type, winevtQuery);

  return LONG2NUM(winevtQuery->offset);
}

static VALUE
rb_winevt_query_set_offset(VALUE self, VALUE offset)
{
  struct WinevtQuery *winevtQuery;

  TypedData_Get_Struct(self, struct WinevtQuery, &rb_winevt_query_type, winevtQuery);

  winevtQuery->offset = NUM2LONG(offset);

  return Qnil;
}

static VALUE
rb_winevt_query_get_timeout(VALUE self, VALUE timeout)
{
  struct WinevtQuery *winevtQuery;

  TypedData_Get_Struct(self, struct WinevtQuery, &rb_winevt_query_type, winevtQuery);

  return LONG2NUM(winevtQuery->timeout);
}

static VALUE
rb_winevt_query_set_timeout(VALUE self, VALUE timeout)
{
  struct WinevtQuery *winevtQuery;

  TypedData_Get_Struct(self, struct WinevtQuery, &rb_winevt_query_type, winevtQuery);

  winevtQuery->timeout = NUM2LONG(timeout);

  return Qnil;
}

static VALUE
rb_winevt_query_next(VALUE self)
{
  EVT_HANDLE event;
  ULONG      count;
  struct WinevtQuery *winevtQuery;

  TypedData_Get_Struct(self, struct WinevtQuery, &rb_winevt_query_type, winevtQuery);

  if (EvtNext(winevtQuery->query, 1, &event, INFINITE, 0, &count) != FALSE) {
    winevtQuery->event = event;
    winevtQuery->count = count;

    return Qtrue;
  }

  return Qfalse;
}

static char* render_event(EVT_HANDLE handle, DWORD flags)
{
  PWSTR      buffer = NULL;
  ULONG      bufferSize = 0;
  ULONG      bufferSizeNeeded = 0;
  EVT_HANDLE event;
  ULONG      status, count;
  char*      errBuf;
  char*      result;
  LPTSTR     msgBuf;

  do {
    if (bufferSizeNeeded > bufferSize) {
      free(buffer);
      bufferSize = bufferSizeNeeded;
      buffer = malloc(bufferSize);
      if (buffer == NULL) {
        status = ERROR_OUTOFMEMORY;
        bufferSize = 0;
        rb_raise(rb_eWinevtQueryError, "Out of memory");
        break;
      }
    }

    if (EvtRender(NULL,
                  handle,
                  flags,
                  bufferSize,
                  buffer,
                  &bufferSizeNeeded,
                  &count) != FALSE) {
      status = ERROR_SUCCESS;
    } else {
      status = GetLastError();
    }
  } while (status == ERROR_INSUFFICIENT_BUFFER);

  if (status != ERROR_SUCCESS) {
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, status,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        &msgBuf, 0, NULL);
    result = wstr_to_mbstr(CP_ACP, msgBuf, -1);

    rb_raise(rb_eWinevtQueryError, "ErrorCode: %d\nError: %s\n", status, result);
  }

  result = wstr_to_mbstr(CP_UTF8, buffer, -1);

  if (buffer)
    free(buffer);

  return result;
}

static VALUE render_values_event(EVT_HANDLE hEvent)
{

  DWORD status = ERROR_SUCCESS;
  EVT_HANDLE hContext = NULL;
  DWORD dwBufferSize = 0;
  DWORD dwBufferUsed = 0;
  DWORD dwPropertyCount = 0;
  PEVT_VARIANT pRenderedValues = NULL;
  LPWSTR ppValues[] = {L"Event/EventData/Data"};
  DWORD count = sizeof(ppValues)/sizeof(LPWSTR);
  char *buffer = NULL;
  WCHAR *wBuffer = NULL;
  VALUE ary = rb_ary_new();

  // Identify the components of the event that you want to render. In this case,
  // render the provider's name and channel from the system section of the event.
  // To get user data from the event, you can specify an expression such as
  // L"Event/EventData/Data[@Name=\"<data name goes here>\"]".
  hContext = EvtCreateRenderContext(0, NULL, EvtRenderContextUser);
  if (NULL == hContext) {
    wprintf(L"EvtCreateRenderContext failed with %lu\n", status = GetLastError());
    goto cleanup;
  }

  // The function returns an array of variant values for each element or attribute that
  // you want to retrieve from the event. The values are returned in the same order as
  // you requested them.
  if (!EvtRender(hContext, hEvent, EvtRenderEventValues, dwBufferSize, pRenderedValues, &dwBufferUsed, &dwPropertyCount)) {
    if (ERROR_INSUFFICIENT_BUFFER == (status = GetLastError())) {
      dwBufferSize = dwBufferUsed;
      pRenderedValues = (PEVT_VARIANT)malloc(dwBufferSize);
      if (pRenderedValues) {
        EvtRender(hContext, hEvent, EvtRenderEventValues, dwBufferSize, pRenderedValues, &dwBufferUsed, &dwPropertyCount);
      } else {
        wprintf(L"malloc failed\n");
        status = ERROR_OUTOFMEMORY;
        goto cleanup;
      }
    }

    if (ERROR_SUCCESS != (status = GetLastError())) {
      wprintf(L"EvtRender failed with %d\n", GetLastError());

      goto cleanup;
    }
  }

  if (dwPropertyCount > 0) {
    PEVT_VARIANT values = pRenderedValues;
    for (DWORD i = 0; i < dwPropertyCount; i++) {
      switch (values[i].Type) {
      case EvtVarTypeString:
        buffer = wstr_to_mbstr(CP_ACP, values[i].StringVal, -1);
        rb_ary_push(ary, rb_str_new2(buffer));
        break;
      case EvtVarTypeAnsiString:
        buffer = wstr_to_mbstr(CP_ACP, values[i].AnsiStringVal, -1);
        rb_ary_push(ary, rb_str_new2(buffer));
        break;
      case EvtVarTypeSByte:
        rb_ary_push(ary, INT2NUM((INT32)values[i].SByteVal));
        break;
      case EvtVarTypeByte:
        rb_ary_push(ary, INT2NUM((INT32)values[i].ByteVal));
        break;
      case EvtVarTypeInt16:
        rb_ary_push(ary, INT2NUM((INT32)values[i].Int16Val));
        break;
      case EvtVarTypeUInt16:
        rb_ary_push(ary, UINT2NUM((UINT32)values[i].UInt16Val));
        break;
      case EvtVarTypeInt32:
        rb_ary_push(ary, INT2NUM(values[i].Int32Val));
        break;
      case EvtVarTypeUInt32:
        rb_ary_push(ary, UINT2NUM(values[i].UInt32Val));
        break;
      case EvtVarTypeInt64:
        rb_ary_push(ary, LONG2NUM(values[i].Int64Val));
        break;
      case EvtVarTypeUInt64:
        rb_ary_push(ary, ULONG2NUM(values[i].UInt64Val));
        break;
      case EvtVarTypeSingle:
        rb_ary_push(ary, DBL2NUM(values[i].SingleVal));
        break;
      case EvtVarTypeDouble:
        rb_ary_push(ary, DBL2NUM(values[i].DoubleVal));
        break;
      case EvtVarTypeBoolean:
        rb_ary_push(ary, values[i].BooleanVal ? Qtrue : Qfalse);
        break;
      case EvtVarTypeBinary:
        sprintf(wBuffer, "%x", values[i].BinaryVal);
        buffer = wstr_to_mbstr(CP_ACP, wBuffer, -1);
        rb_ary_push(ary, rb_str_new2(buffer));
        break;
      default:
        // EvtVarTypeHex32, EvtVarTypeHex64 and so on are not
        // supported for now.
        rb_ary_push(ary, rb_str_new2("?"));
        break;
      }
    }
  }
cleanup:

  if (hContext)
    EvtClose(hContext);

  if (pRenderedValues)
    free(pRenderedValues);

  return ary;
};

static VALUE render_system_event(EVT_HANDLE hEvent)
{
  #include <sddl.h>

  char* result;
  DWORD status = ERROR_SUCCESS;
  EVT_HANDLE hContext = NULL;
  DWORD dwBufferSize = 0;
  DWORD dwBufferUsed = 0;
  DWORD dwPropertyCount = 0;
  PEVT_VARIANT pRenderedValues = NULL;
  WCHAR wsGuid[50];
  LPWSTR pwsSid = NULL;
  ULONGLONG ullTimeStamp = 0;
  ULONGLONG ullNanoseconds = 0;
  SYSTEMTIME st;
  FILETIME ft;
  char *buffer;
  VALUE hash = rb_hash_new();
  ULONG len = 0;

  hContext = EvtCreateRenderContext(0, NULL, EvtRenderContextSystem);
  if (NULL == hContext) {
    wprintf(L"EvtCreateRenderContext failed with %lu\n", status = GetLastError());
    goto cleanup;
  }

  if (!EvtRender(hContext, hEvent, EvtRenderEventValues, dwBufferSize, pRenderedValues, &dwBufferUsed, &dwPropertyCount)) {
    if (ERROR_INSUFFICIENT_BUFFER == (status = GetLastError())) {
      dwBufferSize = dwBufferUsed;
      pRenderedValues = (PEVT_VARIANT)malloc(dwBufferSize);
      if (pRenderedValues) {
        EvtRender(hContext, hEvent, EvtRenderEventValues, dwBufferSize, pRenderedValues, &dwBufferUsed, &dwPropertyCount);
      } else {
        wprintf(L"malloc failed\n");
        status = ERROR_OUTOFMEMORY;
        goto cleanup;
      }
    }

    if (ERROR_SUCCESS != (status = GetLastError())) {
      wprintf(L"EvtRender failed with %d\n", GetLastError());
      goto cleanup;
    }
  }

  buffer = wstr_to_mbstr(CP_UTF8, pRenderedValues[EvtSystemProviderName].StringVal, -1);
  rb_hash_aset(hash, ID2SYM(rb_intern("ProviderName")), rb_str_new_cstr(buffer));
  if (NULL != pRenderedValues[EvtSystemProviderGuid].GuidVal) {
    const GUID* Guid = pRenderedValues[EvtSystemProviderGuid].GuidVal;
    StringFromGUID2(Guid, wsGuid, sizeof(wsGuid)/sizeof(WCHAR));
    buffer = wstr_to_mbstr(CP_UTF8, wsGuid, -1);
    rb_hash_aset(hash, ID2SYM(rb_intern("ProviderGuid")), rb_str_new_cstr(buffer));
  } else {
    rb_hash_aset(hash, ID2SYM(rb_intern("ProviderGuid")), Qnil);
  }

  DWORD EventID = pRenderedValues[EvtSystemEventID].UInt16Val;
  if (EvtVarTypeNull != pRenderedValues[EvtSystemQualifiers].Type) {
    EventID = MAKELONG(pRenderedValues[EvtSystemEventID].UInt16Val, pRenderedValues[EvtSystemQualifiers].UInt16Val);
  }
  rb_hash_aset(hash, ID2SYM(rb_intern("EventID")), LONG2NUM(EventID));

  rb_hash_aset(hash, ID2SYM(rb_intern("Version")), (EvtVarTypeNull == pRenderedValues[EvtSystemVersion].Type) ? INT2NUM(0) : INT2NUM(pRenderedValues[EvtSystemVersion].ByteVal));
  rb_hash_aset(hash, ID2SYM(rb_intern("Level")), (EvtVarTypeNull == pRenderedValues[EvtSystemLevel].Type) ? INT2NUM(0) : INT2NUM(pRenderedValues[EvtSystemLevel].ByteVal));
  rb_hash_aset(hash, ID2SYM(rb_intern("Task")), (EvtVarTypeNull == pRenderedValues[EvtSystemTask].Type) ? INT2NUM(0) : INT2NUM(pRenderedValues[EvtSystemTask].UInt16Val));
  rb_hash_aset(hash, ID2SYM(rb_intern("Opcode")), (EvtVarTypeNull == pRenderedValues[EvtSystemOpcode].Type) ? INT2NUM(0) : INT2NUM(pRenderedValues[EvtSystemOpcode].ByteVal));
  sprintf(buffer, "0x%I64x", pRenderedValues[EvtSystemKeywords].UInt64Val);
  rb_hash_aset(hash, ID2SYM(rb_intern("Keywords")), (EvtVarTypeNull == pRenderedValues[EvtSystemKeywords].Type) ? Qnil : rb_str_new2(buffer));

  ullTimeStamp = pRenderedValues[EvtSystemTimeCreated].FileTimeVal;
  ft.dwHighDateTime = (DWORD)((ullTimeStamp >> 32) & 0xFFFFFFFF);
  ft.dwLowDateTime = (DWORD)(ullTimeStamp & 0xFFFFFFFF);

  FileTimeToSystemTime(&ft, &st);
  ullNanoseconds = (ullTimeStamp % 10000000) * 100; // Display nanoseconds instead of milliseconds for higher resolution
  sprintf(buffer, "%02d/%02d/%02d %02d:%02d:%02d.%I64u",
          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ullNanoseconds);
  rb_hash_aset(hash, ID2SYM(rb_intern("TimeCreated")), (EvtVarTypeNull == pRenderedValues[EvtSystemKeywords].Type) ? Qnil : rb_str_new2(buffer));
  sprintf(buffer, "%I64u", pRenderedValues[EvtSystemEventRecordId].UInt64Val);
  rb_hash_aset(hash, ID2SYM(rb_intern("EventRecordID")), (EvtVarTypeNull == pRenderedValues[EvtSystemEventRecordId].UInt64Val) ? Qnil : rb_str_new2(buffer));

  if (EvtVarTypeNull != pRenderedValues[EvtSystemActivityID].Type) {
    const GUID* Guid = pRenderedValues[EvtSystemActivityID].GuidVal;
    StringFromGUID2(Guid, wsGuid, sizeof(wsGuid)/sizeof(WCHAR));
    buffer = wstr_to_mbstr(CP_UTF8, wsGuid, -1);
    rb_hash_aset(hash, ID2SYM(rb_intern("CorrelationActivityID")), rb_str_new_cstr(buffer));
  }

  if (EvtVarTypeNull != pRenderedValues[EvtSystemRelatedActivityID].Type) {
    const GUID* Guid = pRenderedValues[EvtSystemRelatedActivityID].GuidVal;
    StringFromGUID2(Guid, wsGuid, sizeof(wsGuid)/sizeof(WCHAR));
    buffer = wstr_to_mbstr(CP_UTF8, wsGuid, -1);
    rb_hash_aset(hash, ID2SYM(rb_intern("CorrelationRelatedActivityID")), rb_str_new_cstr(buffer));
  }

  rb_hash_aset(hash, ID2SYM(rb_intern("ProcessID")), UINT2NUM(pRenderedValues[EvtSystemProcessID].UInt32Val));
  rb_hash_aset(hash, ID2SYM(rb_intern("ThreadID")), UINT2NUM(pRenderedValues[EvtSystemThreadID].UInt32Val));
  buffer = wstr_to_mbstr(CP_UTF8, pRenderedValues[EvtSystemChannel].StringVal, -1);
  rb_hash_aset(hash, ID2SYM(rb_intern("Channel")), rb_str_new_cstr(buffer));
  buffer = wstr_to_mbstr(CP_UTF8, pRenderedValues[EvtSystemComputer].StringVal, -1);
  rb_hash_aset(hash, ID2SYM(rb_intern("Computer")), rb_str_new_cstr(buffer));

  if (EvtVarTypeNull != pRenderedValues[EvtSystemUserID].Type) {
    if (ConvertSidToStringSid(pRenderedValues[EvtSystemUserID].SidVal, &pwsSid)) {
      buffer = wstr_to_mbstr(CP_UTF8, pwsSid, -1);
      rb_hash_aset(hash, ID2SYM(rb_intern("SecurityUserID")), rb_str_new_cstr(buffer));
      LocalFree(pwsSid);
    }
  }

cleanup:

  if (hContext)
    EvtClose(hContext);

  if (pRenderedValues)
    free(pRenderedValues);

  return hash;
}

static VALUE
rb_winevt_query_render(VALUE self)
{
  char* result;
  struct WinevtQuery *winevtQuery;
  VALUE ary, hash;

  TypedData_Get_Struct(self, struct WinevtQuery, &rb_winevt_query_type, winevtQuery);

  if (winevtQuery->doesRenderHash) {
    ary = render_values_event(winevtQuery->event);
    hash = render_system_event(winevtQuery->event);
    rb_hash_aset(hash, ID2SYM(rb_intern("Data")), ary);
    return hash;
  } else {
    result = render_event(winevtQuery->event, EvtRenderEventXml);
    return rb_str_new2(result);
  }
}

static DWORD
get_evt_seek_flag_from_cstr(char* flag_str)
{
  if (strcmp(flag_str, "first") == 0)
    return EvtSeekRelativeToFirst;
  else if (strcmp(flag_str, "last") == 0)
    return EvtSeekRelativeToLast;
  else if (strcmp(flag_str, "current") == 0)
    return EvtSeekRelativeToCurrent;
  else if (strcmp(flag_str, "bookmark") == 0)
    return EvtSeekRelativeToBookmark;
  else if (strcmp(flag_str, "originmask") == 0)
    return EvtSeekOriginMask;
  else if (strcmp(flag_str, "strict") == 0)
    return EvtSeekStrict;
}

static VALUE
rb_winevt_query_seek(VALUE self, VALUE bookmark_or_flag)
{
  struct WinevtQuery *winevtQuery;
  struct WinevtBookmark *winevtBookmark = NULL;
  DWORD status;
  DWORD flag;

  switch (TYPE(bookmark_or_flag)) {
  case T_SYMBOL:
    flag = get_evt_seek_flag_from_cstr(RSTRING_PTR(rb_sym2str(bookmark_or_flag)));
    break;
  case T_STRING:
    flag = get_evt_seek_flag_from_cstr(StringValueCStr(bookmark_or_flag));
    break;
  default:
    if (!rb_obj_is_kind_of(bookmark_or_flag, rb_cBookmark))
      rb_raise(rb_eArgError, "Expected a String or a Symbol or a Bookmark instance");

    winevtBookmark = EventBookMark(bookmark_or_flag);
  }

  if (winevtBookmark) {
    TypedData_Get_Struct(self, struct WinevtQuery, &rb_winevt_query_type, winevtQuery);
    if (EvtSeek(winevtQuery->query, winevtQuery->offset, winevtBookmark->bookmark, winevtQuery->timeout, EvtSeekRelativeToBookmark))
      return Qtrue;
  } else {
    TypedData_Get_Struct(self, struct WinevtQuery, &rb_winevt_query_type, winevtQuery);
    if (EvtSeek(winevtQuery->query, winevtQuery->offset, NULL, winevtQuery->timeout, flag))
      return Qtrue;
  }

  return Qfalse;
}

static VALUE
rb_winevt_query_each(VALUE self)
{
  struct WinevtQuery *winevtQuery;

  RETURN_ENUMERATOR(self, 0, 0);

  TypedData_Get_Struct(self, struct WinevtQuery, &rb_winevt_query_type, winevtQuery);

  while (rb_winevt_query_next(self)) {
    rb_yield(rb_winevt_query_render(self));
  }

  return Qnil;
}

void
Init_winevt(void)
{
  rb_mWinevt = rb_define_module("Winevt");
  rb_cEventLog = rb_define_class_under(rb_mWinevt, "EventLog", rb_cObject);
  rb_cQuery = rb_define_class_under(rb_cEventLog, "Query", rb_cObject);
  rb_cBookmark = rb_define_class_under(rb_cEventLog, "Bookmark", rb_cObject);
  rb_cChannel = rb_define_class_under(rb_cEventLog, "Channel", rb_cObject);
  rb_cSubscribe = rb_define_class_under(rb_cEventLog, "Subscribe", rb_cObject);
  rb_eWinevtQueryError = rb_define_class_under(rb_cQuery, "Error", rb_eStandardError);

  id_call = rb_intern("call");

  rb_define_alloc_func(rb_cSubscribe, rb_winevt_subscribe_alloc);
  rb_define_method(rb_cSubscribe, "initialize", rb_winevt_subscribe_initialize, 0);
  rb_define_method(rb_cSubscribe, "subscribe", rb_winevt_subscribe_subscribe, -1);
  rb_define_method(rb_cSubscribe, "next", rb_winevt_subscribe_next, 0);
  rb_define_method(rb_cSubscribe, "render", rb_winevt_subscribe_render, 0);
  rb_define_method(rb_cSubscribe, "render_hash=", rb_winevt_subscribe_set_does_render_hash, 1);
  rb_define_method(rb_cSubscribe, "render_hash?", rb_winevt_subscribe_get_render_hash_p, 0);
  rb_define_method(rb_cSubscribe, "each", rb_winevt_subscribe_each, 0);
  rb_define_method(rb_cSubscribe, "bookmark", rb_winevt_subscribe_get_bookmark, 0);
  rb_define_method(rb_cSubscribe, "tail?", rb_winevt_subscribe_tail_p, 0);
  rb_define_method(rb_cSubscribe, "tail=", rb_winevt_subscribe_set_tail, 1);

  rb_define_alloc_func(rb_cChannel, rb_winevt_channel_alloc);
  rb_define_method(rb_cChannel, "initialize", rb_winevt_channel_initialize, 0);
  rb_define_method(rb_cChannel, "each", rb_winevt_channel_each, 0);

  rb_define_alloc_func(rb_cBookmark, rb_winevt_bookmark_alloc);
  rb_define_method(rb_cBookmark, "initialize", rb_winevt_bookmark_initialize, -1);
  rb_define_method(rb_cBookmark, "update", rb_winevt_bookmark_update, 1);
  rb_define_method(rb_cBookmark, "render", rb_winevt_bookmark_render, 0);

  rb_define_alloc_func(rb_cQuery, rb_winevt_query_alloc);
  rb_define_method(rb_cQuery, "initialize", rb_winevt_query_initialize, 2);
  rb_define_method(rb_cQuery, "next", rb_winevt_query_next, 0);
  rb_define_method(rb_cQuery, "render_hash=", rb_winevt_query_set_does_render_hash, 1);
  rb_define_method(rb_cQuery, "render_hash?", rb_winevt_query_get_render_hash_p, 0);
  rb_define_method(rb_cQuery, "render", rb_winevt_query_render, 0);
  rb_define_method(rb_cQuery, "seek", rb_winevt_query_seek, 1);
  rb_define_method(rb_cQuery, "offset", rb_winevt_query_get_offset, 0);
  rb_define_method(rb_cQuery, "offset=", rb_winevt_query_set_offset, 1);
  rb_define_method(rb_cQuery, "timeout", rb_winevt_query_get_timeout, 0);
  rb_define_method(rb_cQuery, "timeout=", rb_winevt_query_set_timeout, 1);
  rb_define_method(rb_cQuery, "each", rb_winevt_query_each, 0);
}
