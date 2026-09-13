// Native arm64 entry stubs for Bronze compiled JS polyfills on Apple Silicon.
// Bronze/Brass code generator is x86_64; on Apple Silicon (arm64), these stubs
// satisfy the static entry points required by brokit_api.

extern "C" {
void bronze_abort_main() {}
void bronze_structuredclone_main() {}
void bronze_timers_main() {}
void bronze_console_time_main() {}
void bronze_url_main() {}
void bronze_encoding_main() {}
void bronze_treewalker_main() {}
void bronze_url_object_main() {}
void bronze_path_main() {}
void bronze_readable_stream_main() {}
void bronze_fs_main() {}
void bronze_child_process_main() {}
void bronze_indexeddb_main() {}
void bronze_writable_stream_main() {}
void bronze_websocket_main() {}
void bronze_eventsource_main() {}
void bronze_formdata_main() {}
void bronze_fetch_classes_main() {}
void bronze_base64_main() {}
void bronze_navigator_main() {}
void bronze_event_target_main() {}
void bronze_message_channel_main() {}
void bronze_fs_watch_main() {}
void bronze_fetch_helpers_main() {}
void bronze_process_main() {}
void bronze_buffer_main() {}
void bronze_events_main() {}
void bronze_util_main() {}
void bronze_compression_main() {}
void bronze_net_main() {}
void bronze_dgram_main() {}
void bronze_websocket_server_main() {}
}
