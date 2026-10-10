// Minimal libmpv client API surface, resolved dynamically from libmpv-2.dll.
// Mirrors <mpv/client.h> (API 2.x) for exactly the entry points and structs
// this plugin touches. Keeping our own declarations means the plugin builds
// without the mpv SDK and degrades gracefully (plugin stays idle) when the
// DLL is missing at runtime, instead of failing to load the DLL.
#pragma once
#include <windows.h>
#include <stdint.h>

extern "C" {

typedef struct mpv_handle mpv_handle;

enum mpv_format {
  MPV_FORMAT_NONE       = 0,
  MPV_FORMAT_STRING     = 1,
  MPV_FORMAT_OSD_STRING = 2,
  MPV_FORMAT_FLAG       = 3,
  MPV_FORMAT_INT64      = 4,
  MPV_FORMAT_DOUBLE     = 5,
  MPV_FORMAT_NODE       = 6,
  MPV_FORMAT_NODE_ARRAY = 7,
  MPV_FORMAT_NODE_MAP   = 8,
  MPV_FORMAT_BYTE_ARRAY = 9
};

enum mpv_event_id {
  MPV_EVENT_NONE               = 0,
  MPV_EVENT_SHUTDOWN           = 1,
  MPV_EVENT_LOG_MESSAGE        = 2,
  MPV_EVENT_GET_PROPERTY_REPLY = 3,
  MPV_EVENT_SET_PROPERTY_REPLY = 4,
  MPV_EVENT_COMMAND_REPLY      = 5,
  MPV_EVENT_START_FILE         = 6,
  MPV_EVENT_END_FILE           = 7,
  MPV_EVENT_FILE_LOADED        = 8,
  MPV_EVENT_CLIENT_MESSAGE     = 16,
  MPV_EVENT_VIDEO_RECONFIG     = 17,
  MPV_EVENT_AUDIO_RECONFIG     = 18,
  MPV_EVENT_SEEK               = 20,
  MPV_EVENT_PLAYBACK_RESTART   = 21,
  MPV_EVENT_PROPERTY_CHANGE    = 22,
  MPV_EVENT_QUEUE_OVERFLOW     = 24,
  MPV_EVENT_HOOK               = 25
};

enum mpv_end_file_reason {
  MPV_END_FILE_REASON_EOF      = 0,
  MPV_END_FILE_REASON_STOP     = 2,
  MPV_END_FILE_REASON_QUIT     = 3,
  MPV_END_FILE_REASON_ERROR    = 4,
  MPV_END_FILE_REASON_REDIRECT = 5
};

typedef struct mpv_event_property {
  const char *name;
  mpv_format  format;
  void       *data;
} mpv_event_property;

typedef struct mpv_event_end_file {
  int     reason;
  int     error;
  int64_t playlist_entry_id;
  int     playlist_insert_id;
  int     playlist_insert_num_entries;
} mpv_event_end_file;

typedef struct mpv_event_log_message {
  const char *prefix;
  const char *level;
  const char *text;
  int         log_level;
} mpv_event_log_message;

typedef struct mpv_event {
  mpv_event_id event_id;
  int          error;
  uint64_t     reply_userdata;
  void        *data;
} mpv_event;

enum { MPV_ERROR_LOADING_FAILED = -13, MPV_ERROR_GENERIC = -20 };

// <mpv/stream_cb.h>: a read-only stream protocol implemented by the client.
typedef int64_t (*mpv_stream_cb_read_fn)(void *cookie, char *buf, uint64_t nbytes);
typedef int64_t (*mpv_stream_cb_seek_fn)(void *cookie, int64_t offset);
typedef int64_t (*mpv_stream_cb_size_fn)(void *cookie);
typedef void    (*mpv_stream_cb_close_fn)(void *cookie);
typedef void    (*mpv_stream_cb_cancel_fn)(void *cookie);
typedef struct mpv_stream_cb_info {
  void *cookie;
  mpv_stream_cb_read_fn   read_fn;
  mpv_stream_cb_seek_fn   seek_fn;
  mpv_stream_cb_size_fn   size_fn;
  mpv_stream_cb_close_fn  close_fn;
  mpv_stream_cb_cancel_fn cancel_fn;
} mpv_stream_cb_info;
typedef int (*mpv_stream_cb_open_ro_fn)(void *user_data, char *uri, mpv_stream_cb_info *info);

typedef unsigned long (*pfn_mpv_client_api_version)(void);
typedef mpv_handle *(*pfn_mpv_create)(void);
typedef int         (*pfn_mpv_initialize)(mpv_handle *);
typedef void        (*pfn_mpv_terminate_destroy)(mpv_handle *);
typedef int         (*pfn_mpv_set_option_string)(mpv_handle *, const char *, const char *);
typedef int         (*pfn_mpv_command)(mpv_handle *, const char **);
typedef int         (*pfn_mpv_set_property)(mpv_handle *, const char *, mpv_format, void *);
typedef int         (*pfn_mpv_set_property_string)(mpv_handle *, const char *, const char *);
typedef int         (*pfn_mpv_get_property)(mpv_handle *, const char *, mpv_format, void *);
typedef char       *(*pfn_mpv_get_property_string)(mpv_handle *, const char *);
typedef void        (*pfn_mpv_free)(void *);
typedef int         (*pfn_mpv_observe_property)(mpv_handle *, uint64_t, const char *, mpv_format);
typedef mpv_event  *(*pfn_mpv_wait_event)(mpv_handle *, double);
typedef void        (*pfn_mpv_set_wakeup_callback)(mpv_handle *, void (*)(void *), void *);
typedef const char *(*pfn_mpv_error_string)(int);
typedef int         (*pfn_mpv_request_log_messages)(mpv_handle *, const char *);
typedef int         (*pfn_mpv_stream_cb_add_ro)(mpv_handle *, const char *, void *, mpv_stream_cb_open_ro_fn);

} // extern "C"

struct MpvApi {
  HMODULE                        hModule = nullptr;
  pfn_mpv_client_api_version     client_api_version = nullptr;
  pfn_mpv_create                 create = nullptr;
  pfn_mpv_initialize             initialize = nullptr;
  pfn_mpv_terminate_destroy      terminate_destroy = nullptr;
  pfn_mpv_set_option_string      set_option_string = nullptr;
  pfn_mpv_command                command = nullptr;
  pfn_mpv_set_property           set_property = nullptr;
  pfn_mpv_set_property_string    set_property_string = nullptr;
  pfn_mpv_get_property           get_property = nullptr;
  pfn_mpv_get_property_string    get_property_string = nullptr;
  pfn_mpv_free                   free = nullptr;
  pfn_mpv_observe_property       observe_property = nullptr;
  pfn_mpv_wait_event             wait_event = nullptr;
  pfn_mpv_set_wakeup_callback    set_wakeup_callback = nullptr;
  pfn_mpv_error_string           error_string = nullptr;
  pfn_mpv_request_log_messages   request_log_messages = nullptr;
  pfn_mpv_stream_cb_add_ro       stream_cb_add_ro = nullptr;

  // Loads libmpv-2.dll from the given directory (the plugin's own folder).
  // Returns false, leaving the struct empty, if the DLL or any symbol is missing.
  bool Load(const wchar_t *dir);
  void Unload();
  bool Loaded() const { return hModule != nullptr; }
};
