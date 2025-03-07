// SPDX-License-Identifier: GPL-3.0-or-later

#include "web_client.h"

// this is an async I/O implementation of the web server request parser
// it is used by all netdata web servers

int respect_web_browser_do_not_track_policy = 0;
char *web_x_frame_options = NULL;

int web_enable_gzip = 1, web_gzip_level = 3, web_gzip_strategy = Z_DEFAULT_STRATEGY;

inline int web_client_permission_denied(struct web_client *w) {
    w->response.data->content_type = CT_TEXT_PLAIN;
    buffer_flush(w->response.data);
    buffer_strcat(w->response.data, "You are not allowed to access this resource.");
    w->response.code = HTTP_RESP_FORBIDDEN;
    return HTTP_RESP_FORBIDDEN;
}

inline int web_client_bearer_required(struct web_client *w) {
    w->response.data->content_type = CT_TEXT_PLAIN;
    buffer_flush(w->response.data);
    buffer_strcat(w->response.data, "An authorization bearer is required to access the resource.");
    w->response.code = HTTP_RESP_PRECOND_FAIL;
    return HTTP_RESP_PRECOND_FAIL;
}

static inline int bad_request_multiple_dashboard_versions(struct web_client *w) {
    w->response.data->content_type = CT_TEXT_PLAIN;
    buffer_flush(w->response.data);
    buffer_strcat(w->response.data, "Multiple dashboard versions given at the URL.");
    w->response.code = HTTP_RESP_BAD_REQUEST;
    return HTTP_RESP_BAD_REQUEST;
}

static inline int web_client_cork_socket(struct web_client *w __maybe_unused) {
#ifdef TCP_CORK
    if(likely(web_client_is_corkable(w) && !w->tcp_cork && w->ofd != -1)) {
        w->tcp_cork = true;
        if(unlikely(setsockopt(w->ofd, IPPROTO_TCP, TCP_CORK, (char *) &w->tcp_cork, sizeof(int)) != 0)) {
            netdata_log_error("%llu: failed to enable TCP_CORK on socket.", w->id);

            w->tcp_cork = false;
            return -1;
        }
    }
#endif /* TCP_CORK */

    return 0;
}

static inline void web_client_enable_wait_from_ssl(struct web_client *w) {
    if (w->ssl.ssl_errno == SSL_ERROR_WANT_READ)
        web_client_enable_ssl_wait_receive(w);
    else if (w->ssl.ssl_errno == SSL_ERROR_WANT_WRITE)
        web_client_enable_ssl_wait_send(w);
    else {
        web_client_disable_ssl_wait_receive(w);
        web_client_disable_ssl_wait_send(w);
    }
}

static inline int web_client_uncork_socket(struct web_client *w __maybe_unused) {
#ifdef TCP_CORK
    if(likely(w->tcp_cork && w->ofd != -1)) {
        w->tcp_cork = false;
        if(unlikely(setsockopt(w->ofd, IPPROTO_TCP, TCP_CORK, (char *) &w->tcp_cork, sizeof(int)) != 0)) {
            netdata_log_error("%llu: failed to disable TCP_CORK on socket.", w->id);
            w->tcp_cork = true;
            return -1;
        }
    }
#endif /* TCP_CORK */

    w->tcp_cork = false;
    return 0;
}

char *strip_control_characters(char *url) {
    char *s = url;
    if(!s) return "";

    if(iscntrl(*s)) *s = ' ';
    while(*++s) {
        if(iscntrl(*s)) *s = ' ';
    }

    return url;
}

static void web_client_reset_allocations(struct web_client *w, bool free_all) {

    if(free_all) {
        // the web client is to be destroyed

        buffer_free(w->url_as_received);
        w->url_as_received = NULL;

        buffer_free(w->url_path_decoded);
        w->url_path_decoded = NULL;

        buffer_free(w->url_query_string_decoded);
        w->url_query_string_decoded = NULL;

        buffer_free(w->response.header_output);
        w->response.header_output = NULL;

        buffer_free(w->response.header);
        w->response.header = NULL;

        buffer_free(w->response.data);
        w->response.data = NULL;

        freez(w->post_payload);
        w->post_payload = NULL;
        w->post_payload_size = 0;
    }
    else {
        // the web client is to be re-used

        buffer_reset(w->url_as_received);
        buffer_reset(w->url_path_decoded);
        buffer_reset(w->url_query_string_decoded);

        buffer_reset(w->response.header_output);
        buffer_reset(w->response.header);
        buffer_reset(w->response.data);

        // leave w->post_payload
    }

    freez(w->server_host);
    w->server_host = NULL;

    freez(w->forwarded_host);
    w->forwarded_host = NULL;

    freez(w->origin);
    w->origin = NULL;

    freez(w->user_agent);
    w->user_agent = NULL;

    freez(w->auth_bearer_token);
    w->auth_bearer_token = NULL;

    // if we had enabled compression, release it
    if(w->response.zinitialized) {
        deflateEnd(&w->response.zstream);
        w->response.zsent = 0;
        w->response.zhave = 0;
        w->response.zstream.avail_in = 0;
        w->response.zstream.avail_out = 0;
        w->response.zstream.total_in = 0;
        w->response.zstream.total_out = 0;
        w->response.zinitialized = false;
        w->flags &= ~WEB_CLIENT_CHUNKED_TRANSFER;
    }

    web_client_reset_path_flags(w);
}

void web_client_request_done(struct web_client *w) {
    web_client_uncork_socket(w);

    netdata_log_debug(D_WEB_CLIENT, "%llu: Resetting client.", w->id);

    if(likely(buffer_strlen(w->url_as_received))) {
        struct timeval tv;
        now_monotonic_high_precision_timeval(&tv);

        size_t size = (w->mode == WEB_CLIENT_MODE_FILECOPY)?w->response.rlen:w->response.data->len;
        size_t sent = size;
        if(likely(w->response.zoutput)) sent = (size_t)w->response.zstream.total_out;

        // --------------------------------------------------------------------
        // global statistics

        global_statistics_web_request_completed(dt_usec(&tv, &w->timings.tv_in),
                                                w->statistics.received_bytes,
                                                w->statistics.sent_bytes,
                                                size,
                                                sent);

        w->statistics.received_bytes = 0;
        w->statistics.sent_bytes = 0;


        // --------------------------------------------------------------------

        const char *mode;
        switch(w->mode) {
            case WEB_CLIENT_MODE_FILECOPY:
                mode = "FILECOPY";
                break;

            case WEB_CLIENT_MODE_OPTIONS:
                mode = "OPTIONS";
                break;

            case WEB_CLIENT_MODE_STREAM:
                mode = "STREAM";
                break;

            case WEB_CLIENT_MODE_POST:
            case WEB_CLIENT_MODE_PUT:
            case WEB_CLIENT_MODE_GET:
            case WEB_CLIENT_MODE_DELETE:
                mode = "DATA";
                break;

            default:
                mode = "UNKNOWN";
                break;
        }

        // access log
        netdata_log_access("%llu: %d '[%s]:%s' '%s' (sent/all = %zu/%zu bytes %0.0f%%, prep/sent/total = %0.2f/%0.2f/%0.2f ms) %d '%s'",
                   w->id
                   , gettid()
                   , w->client_ip
                   , w->client_port
                   , mode
                   , sent
                   , size
                   , -((size > 0) ? ((double)(size - sent) / (double) size * 100.0) : 0.0)
                   , (double)dt_usec(&w->timings.tv_ready, &w->timings.tv_in) / 1000.0
                   , (double)dt_usec(&tv, &w->timings.tv_ready) / 1000.0
                   , (double)dt_usec(&tv, &w->timings.tv_in) / 1000.0
                   , w->response.code
                   , strip_control_characters((char *)buffer_tostring(w->url_as_received))
        );
    }

    if(unlikely(w->mode == WEB_CLIENT_MODE_FILECOPY)) {
        if(w->ifd != w->ofd) {
            netdata_log_debug(D_WEB_CLIENT, "%llu: Closing filecopy input file descriptor %d.", w->id, w->ifd);

            if(web_server_mode != WEB_SERVER_MODE_STATIC_THREADED) {
                if (w->ifd != -1){
                    close(w->ifd);
                }
            }

            w->ifd = w->ofd;
        }
    }

    web_client_reset_allocations(w, false);

    w->mode = WEB_CLIENT_MODE_GET;

    web_client_disable_donottrack(w);
    web_client_disable_tracking_required(w);
    web_client_disable_keepalive(w);

    w->header_parse_tries = 0;
    w->header_parse_last_size = 0;

    web_client_enable_wait_receive(w);
    web_client_disable_wait_send(w);

    w->response.has_cookies = false;
    w->response.rlen = 0;
    w->response.sent = 0;
    w->response.code = 0;
    w->response.zoutput = false;
}

static struct {
    const char *extension;
    uint32_t hash;
    uint8_t contenttype;
} mime_types[] = {
        {  "html" , 0    , CT_TEXT_HTML}
        , {"js"   , 0    , CT_APPLICATION_X_JAVASCRIPT}
        , {"css"  , 0    , CT_TEXT_CSS}
        , {"xml"  , 0    , CT_TEXT_XML}
        , {"xsl"  , 0    , CT_TEXT_XSL}
        , {"txt"  , 0    , CT_TEXT_PLAIN}
        , {"svg"  , 0    , CT_IMAGE_SVG_XML}
        , {"ttf"  , 0    , CT_APPLICATION_X_FONT_TRUETYPE}
        , {"otf"  , 0    , CT_APPLICATION_X_FONT_OPENTYPE}
        , {"woff2", 0    , CT_APPLICATION_FONT_WOFF2}
        , {"woff" , 0    , CT_APPLICATION_FONT_WOFF}
        , {"eot"  , 0    , CT_APPLICATION_VND_MS_FONTOBJ}
        , {"png"  , 0    , CT_IMAGE_PNG}
        , {"jpg"  , 0    , CT_IMAGE_JPG}
        , {"jpeg" , 0    , CT_IMAGE_JPG}
        , {"gif"  , 0    , CT_IMAGE_GIF}
        , {"bmp"  , 0    , CT_IMAGE_BMP}
        , {"ico"  , 0    , CT_IMAGE_XICON}
        , {"icns" , 0    , CT_IMAGE_ICNS}
        , {       NULL, 0, 0}
};

static inline uint8_t contenttype_for_filename(const char *filename) {
    // netdata_log_info("checking filename '%s'", filename);

    static int initialized = 0;
    int i;

    if(unlikely(!initialized)) {
        for (i = 0; mime_types[i].extension; i++)
            mime_types[i].hash = simple_hash(mime_types[i].extension);

        initialized = 1;
    }

    const char *s = filename, *last_dot = NULL;

    // find the last dot
    while(*s) {
        if(unlikely(*s == '.')) last_dot = s;
        s++;
    }

    if(unlikely(!last_dot || !*last_dot || !last_dot[1])) {
        // netdata_log_info("no extension for filename '%s'", filename);
        return CT_APPLICATION_OCTET_STREAM;
    }
    last_dot++;

    // netdata_log_info("extension for filename '%s' is '%s'", filename, last_dot);

    uint32_t hash = simple_hash(last_dot);
    for(i = 0; mime_types[i].extension ; i++) {
        if(unlikely(hash == mime_types[i].hash && !strcmp(last_dot, mime_types[i].extension))) {
            // netdata_log_info("matched extension for filename '%s': '%s'", filename, last_dot);
            return mime_types[i].contenttype;
        }
    }

    // netdata_log_info("not matched extension for filename '%s': '%s'", filename, last_dot);
    return CT_APPLICATION_OCTET_STREAM;
}

static int append_slash_to_url_and_redirect(struct web_client *w) {
    // this function returns a relative redirect
    // it finds the last path component on the URL and just appends / to it
    //
    // So, if the URL is:
    //
    //        /path/to/file?query_string
    //
    // It adds a Location header like this:
    //
    //       Location: file/?query_string\r\n
    //
    // The web browser already knows that it is inside /path/to/
    // so it converts the path to /path/to/file/ and executes the
    // request again.

    buffer_strcat(w->response.header, "Location: ");
    const char *b = buffer_tostring(w->url_as_received);
    const char *q = strchr(b, '?');
    if(q && q > b) {
        const char *e = q - 1;
        while(e > b && *e != '/') e--;
        if(*e == '/') e++;

        size_t len = q - e;
        buffer_strncat(w->response.header, e, len);
        buffer_strncat(w->response.header, "/", 1);
        buffer_strcat(w->response.header, q);
    }
    else {
        const char *e = &b[buffer_strlen(w->url_as_received) - 1];
        while(e > b && *e != '/') e--;
        if(*e == '/') e++;

        buffer_strcat(w->response.header, e);
        buffer_strncat(w->response.header, "/", 1);
    }

    buffer_strncat(w->response.header, "\r\n", 2);

    w->response.data->content_type = CT_TEXT_HTML;
    buffer_flush(w->response.data);
    buffer_strcat(w->response.data,
                  "<!DOCTYPE html><html>"
                  "<body onload=\"window.location.href = window.location.origin + window.location.pathname + '/' + window.location.search + window.location.hash\">"
                  "Redirecting. In case your browser does not support redirection, please click "
                  "<a onclick=\"window.location.href = window.location.origin + window.location.pathname + '/' + window.location.search + window.location.hash\">here</a>."
                  "</body></html>");
    return HTTP_RESP_MOVED_PERM;
}

// Work around a bug in the CMocka library by removing this function during testing.
#ifndef REMOVE_MYSENDFILE

static inline int dashboard_version(struct web_client *w) {
    if(!web_client_flag_check(w, WEB_CLIENT_FLAG_PATH_WITH_VERSION))
        return -1;

    if(web_client_flag_check(w, WEB_CLIENT_FLAG_PATH_IS_V0))
        return 0;
    if(web_client_flag_check(w, WEB_CLIENT_FLAG_PATH_IS_V1))
        return 1;
    if(web_client_flag_check(w, WEB_CLIENT_FLAG_PATH_IS_V2))
        return 2;

    return -1;
}

static bool find_filename_to_serve(const char *filename, char *dst, size_t dst_len, struct stat *statbuf, struct web_client *w, bool *is_dir) {
    int d_version = dashboard_version(w);
    bool has_extension = web_client_flag_check(w, WEB_CLIENT_FLAG_PATH_HAS_FILE_EXTENSION);

    int fallback = 0;

    if(has_extension) {
        if(d_version == -1)
            snprintfz(dst, dst_len, "%s/%s", netdata_configured_web_dir, filename);
        else {
            // check if the filename or directory exists
            // fallback to the same path without the dashboard version otherwise
            snprintfz(dst, dst_len, "%s/v%d/%s", netdata_configured_web_dir, d_version, filename);
            fallback = 1;
        }
    }
    else if(d_version != -1) {
        if(filename && *filename) {
            // check if the filename exists
            // fallback to /vN/index.html otherwise
            snprintfz(dst, dst_len, "%s/%s", netdata_configured_web_dir, filename);
            fallback = 2;
        }
        else {
            if(filename && *filename)
                web_client_flag_set(w, WEB_CLIENT_FLAG_PATH_HAS_TRAILING_SLASH);
            snprintfz(dst, dst_len, "%s/v%d", netdata_configured_web_dir, d_version);
        }
    }
    else {
        // check if filename exists
        // this is needed to serve {filename}/index.html, in case a user puts a html file into a directory
        // fallback to /index.html otherwise
        snprintfz(dst, dst_len, "%s/%s", netdata_configured_web_dir, filename);
        fallback = 3;
    }

    if (stat(dst, statbuf) != 0) {
        if(fallback == 1) {
            snprintfz(dst, dst_len, "%s/%s", netdata_configured_web_dir, filename);
            if (stat(dst, statbuf) != 0)
                return false;
        }
        else if(fallback == 2) {
            if(filename && *filename)
                web_client_flag_set(w, WEB_CLIENT_FLAG_PATH_HAS_TRAILING_SLASH);
            snprintfz(dst, dst_len, "%s/v%d", netdata_configured_web_dir, d_version);
            if (stat(dst, statbuf) != 0)
                return false;
        }
        else if(fallback == 3) {
            if(filename && *filename)
                web_client_flag_set(w, WEB_CLIENT_FLAG_PATH_HAS_TRAILING_SLASH);
            snprintfz(dst, dst_len, "%s", netdata_configured_web_dir);
            if (stat(dst, statbuf) != 0)
                return false;
        }
        else
            return false;
    }

    if((statbuf->st_mode & S_IFMT) == S_IFDIR) {
        size_t len = strlen(dst);
        if(len > dst_len - 11)
            return false;

        strncpyz(&dst[len], "/index.html", dst_len - len);

        if (stat(dst, statbuf) != 0)
            return false;

        *is_dir = true;
    }

    return true;
}

static int mysendfile(struct web_client *w, char *filename) {
    netdata_log_debug(D_WEB_CLIENT, "%llu: Looking for file '%s/%s'", w->id, netdata_configured_web_dir, filename);

    if(!web_client_can_access_dashboard(w))
        return web_client_permission_denied(w);

    // skip leading slashes
    while (*filename == '/') filename++;

    // if the filename contains "strange" characters, refuse to serve it
    char *s;
    for(s = filename; *s ;s++) {
        if( !isalnum(*s) && *s != '/' && *s != '.' && *s != '-' && *s != '_') {
            netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: File '%s' is not acceptable.", w->id, filename);
            w->response.data->content_type = CT_TEXT_HTML;
            buffer_sprintf(w->response.data, "Filename contains invalid characters: ");
            buffer_strcat_htmlescape(w->response.data, filename);
            return HTTP_RESP_BAD_REQUEST;
        }
    }

    // if the filename contains a double dot refuse to serve it
    if(strstr(filename, "..") != 0) {
        netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: File '%s' is not acceptable.", w->id, filename);
        w->response.data->content_type = CT_TEXT_HTML;
        buffer_strcat(w->response.data, "Relative filenames are not supported: ");
        buffer_strcat_htmlescape(w->response.data, filename);
        return HTTP_RESP_BAD_REQUEST;
    }

    // find the physical file on disk
    bool is_dir = false;
    char web_filename[FILENAME_MAX + 1];
    struct stat statbuf;
    if(!find_filename_to_serve(filename, web_filename, FILENAME_MAX, &statbuf, w, &is_dir)) {
        w->response.data->content_type = CT_TEXT_HTML;
        buffer_strcat(w->response.data, "File does not exist, or is not accessible: ");
        buffer_strcat_htmlescape(w->response.data, web_filename);
        return HTTP_RESP_NOT_FOUND;
    }

    if(is_dir && !web_client_flag_check(w, WEB_CLIENT_FLAG_PATH_HAS_TRAILING_SLASH))
        return append_slash_to_url_and_redirect(w);

    // open the file
    w->ifd = open(web_filename, O_NONBLOCK, O_RDONLY);
    if(w->ifd == -1) {
        w->ifd = w->ofd;

        if(errno == EBUSY || errno == EAGAIN) {
            netdata_log_error("%llu: File '%s' is busy, sending 307 Moved Temporarily to force retry.", w->id, web_filename);
            w->response.data->content_type = CT_TEXT_HTML;
            buffer_sprintf(w->response.header, "Location: /%s\r\n", filename);
            buffer_strcat(w->response.data, "File is currently busy, please try again later: ");
            buffer_strcat_htmlescape(w->response.data, web_filename);
            return HTTP_RESP_REDIR_TEMP;
        }
        else {
            netdata_log_error("%llu: Cannot open file '%s'.", w->id, web_filename);
            w->response.data->content_type = CT_TEXT_HTML;
            buffer_strcat(w->response.data, "Cannot open file: ");
            buffer_strcat_htmlescape(w->response.data, web_filename);
            return HTTP_RESP_NOT_FOUND;
        }
    }

    sock_setnonblock(w->ifd);

    w->response.data->content_type = contenttype_for_filename(web_filename);
    netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: Sending file '%s' (%"PRId64" bytes, ifd %d, ofd %d).", w->id, web_filename, (int64_t)statbuf.st_size, w->ifd, w->ofd);

    w->mode = WEB_CLIENT_MODE_FILECOPY;
    web_client_enable_wait_receive(w);
    web_client_disable_wait_send(w);
    buffer_flush(w->response.data);
    buffer_need_bytes(w->response.data, (size_t)statbuf.st_size);
    w->response.rlen = (size_t)statbuf.st_size;
#ifdef __APPLE__
    w->response.data->date = statbuf.st_mtimespec.tv_sec;
#else
    w->response.data->date = statbuf.st_mtim.tv_sec;
#endif 
    buffer_cacheable(w->response.data);

    return HTTP_RESP_OK;
}
#endif

void web_client_enable_deflate(struct web_client *w, int gzip) {
    if(unlikely(w->response.zinitialized)) {
        netdata_log_debug(D_DEFLATE, "%llu: Compression has already be initialized for this client.", w->id);
        return;
    }

    if(unlikely(w->response.sent)) {
        netdata_log_error("%llu: Cannot enable compression in the middle of a conversation.", w->id);
        return;
    }

    w->response.zstream.zalloc = Z_NULL;
    w->response.zstream.zfree = Z_NULL;
    w->response.zstream.opaque = Z_NULL;

    w->response.zstream.next_in = (Bytef *)w->response.data->buffer;
    w->response.zstream.avail_in = 0;
    w->response.zstream.total_in = 0;

    w->response.zstream.next_out = w->response.zbuffer;
    w->response.zstream.avail_out = 0;
    w->response.zstream.total_out = 0;

    w->response.zstream.zalloc = Z_NULL;
    w->response.zstream.zfree = Z_NULL;
    w->response.zstream.opaque = Z_NULL;

//  if(deflateInit(&w->response.zstream, Z_DEFAULT_COMPRESSION) != Z_OK) {
//      netdata_log_error("%llu: Failed to initialize zlib. Proceeding without compression.", w->id);
//      return;
//  }

    // Select GZIP compression: windowbits = 15 + 16 = 31
    if(deflateInit2(&w->response.zstream, web_gzip_level, Z_DEFLATED, 15 + ((gzip)?16:0), 8, web_gzip_strategy) != Z_OK) {
        netdata_log_error("%llu: Failed to initialize zlib. Proceeding without compression.", w->id);
        return;
    }

    w->response.zsent = 0;
    w->response.zoutput = true;
    w->response.zinitialized = true;
    w->flags |= WEB_CLIENT_CHUNKED_TRANSFER;

    netdata_log_debug(D_DEFLATE, "%llu: Initialized compression.", w->id);
}

void buffer_data_options2string(BUFFER *wb, uint32_t options) {
    int count = 0;

    if(options & RRDR_OPTION_NONZERO) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "nonzero");
    }

    if(options & RRDR_OPTION_REVERSED) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "flip");
    }

    if(options & RRDR_OPTION_JSON_WRAP) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "jsonwrap");
    }

    if(options & RRDR_OPTION_MIN2MAX) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "min2max");
    }

    if(options & RRDR_OPTION_MILLISECONDS) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "ms");
    }

    if(options & RRDR_OPTION_ABSOLUTE) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "absolute");
    }

    if(options & RRDR_OPTION_SECONDS) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "seconds");
    }

    if(options & RRDR_OPTION_NULL2ZERO) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "null2zero");
    }

    if(options & RRDR_OPTION_OBJECTSROWS) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "objectrows");
    }

    if(options & RRDR_OPTION_GOOGLE_JSON) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "google_json");
    }

    if(options & RRDR_OPTION_PERCENTAGE) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "percentage");
    }

    if(options & RRDR_OPTION_NOT_ALIGNED) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "unaligned");
    }

    if(options & RRDR_OPTION_ANOMALY_BIT) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "anomaly-bit");
    }
}

static inline int check_host_and_call(RRDHOST *host, struct web_client *w, char *url, int (*func)(RRDHOST *, struct web_client *, char *)) {
    //if(unlikely(host->rrd_memory_mode == RRD_MEMORY_MODE_NONE)) {
    //    buffer_flush(w->response.data);
    //    buffer_strcat(w->response.data, "This host does not maintain a database");
    //    return HTTP_RESP_BAD_REQUEST;
    //}

    return func(host, w, url);
}

static inline int UNUSED_FUNCTION(check_host_and_dashboard_acl_and_call)(RRDHOST *host, struct web_client *w, char *url, int (*func)(RRDHOST *, struct web_client *, char *)) {
    if(!web_client_can_access_dashboard(w))
        return web_client_permission_denied(w);

    return check_host_and_call(host, w, url, func);
}

static inline int UNUSED_FUNCTION(check_host_and_mgmt_acl_and_call)(RRDHOST *host, struct web_client *w, char *url, int (*func)(RRDHOST *, struct web_client *, char *)) {
    if(!web_client_can_access_mgmt(w))
        return web_client_permission_denied(w);

    return check_host_and_call(host, w, url, func);
}

int web_client_api_request(RRDHOST *host, struct web_client *w, char *url_path_fragment)
{
    // get the api version
    char *tok = strsep_skip_consecutive_separators(&url_path_fragment, "/");
    if(tok && *tok) {
        netdata_log_debug(D_WEB_CLIENT, "%llu: Searching for API version '%s'.", w->id, tok);
        if(strcmp(tok, "v2") == 0)
            return web_client_api_request_v2(host, w, url_path_fragment);
        else if(strcmp(tok, "v1") == 0)
            return web_client_api_request_v1(host, w, url_path_fragment);
        else {
            buffer_flush(w->response.data);
            w->response.data->content_type = CT_TEXT_HTML;
            buffer_strcat(w->response.data, "Unsupported API version: ");
            buffer_strcat_htmlescape(w->response.data, tok);
            return HTTP_RESP_NOT_FOUND;
        }
    }
    else {
        buffer_flush(w->response.data);
        buffer_sprintf(w->response.data, "Which API version?");
        return HTTP_RESP_BAD_REQUEST;
    }
}

const char *web_content_type_to_string(HTTP_CONTENT_TYPE content_type) {
    switch(content_type) {
        case CT_TEXT_HTML:
            return "text/html; charset=utf-8";

        case CT_APPLICATION_XML:
            return "application/xml; charset=utf-8";

        case CT_APPLICATION_JSON:
            return "application/json; charset=utf-8";

        case CT_APPLICATION_X_JAVASCRIPT:
            return "application/javascript; charset=utf-8";

        case CT_TEXT_CSS:
            return "text/css; charset=utf-8";

        case CT_TEXT_XML:
            return "text/xml; charset=utf-8";

        case CT_TEXT_XSL:
            return "text/xsl; charset=utf-8";

        case CT_APPLICATION_OCTET_STREAM:
            return "application/octet-stream";

        case CT_IMAGE_SVG_XML:
            return "image/svg+xml";

        case CT_APPLICATION_X_FONT_TRUETYPE:
            return "application/x-font-truetype";

        case CT_APPLICATION_X_FONT_OPENTYPE:
            return "application/x-font-opentype";

        case CT_APPLICATION_FONT_WOFF:
            return "application/font-woff";

        case CT_APPLICATION_FONT_WOFF2:
            return "application/font-woff2";

        case CT_APPLICATION_VND_MS_FONTOBJ:
            return "application/vnd.ms-fontobject";

        case CT_IMAGE_PNG:
            return "image/png";

        case CT_IMAGE_JPG:
            return "image/jpeg";

        case CT_IMAGE_GIF:
            return "image/gif";

        case CT_IMAGE_XICON:
            return "image/x-icon";

        case CT_IMAGE_BMP:
            return "image/bmp";

        case CT_IMAGE_ICNS:
            return "image/icns";

        case CT_PROMETHEUS:
            return "text/plain; version=0.0.4";

        case CT_AUDIO_MPEG:
            return "audio/mpeg";

        case CT_AUDIO_OGG:
            return "audio/ogg";

        case CT_VIDEO_MP4:
            return "video/mp4";

        case CT_APPLICATION_PDF:
            return "application/pdf";

        case CT_APPLICATION_ZIP:
            return "application/zip";

        default:
        case CT_TEXT_PLAIN:
            return "text/plain; charset=utf-8";
    }
}

const char *web_response_code_to_string(int code) {
    switch(code) {
        case 100:
            return "Continue";
        case 101:
            return "Switching Protocols";
        case 102:
            return "Processing";
        case 103:
            return "Early Hints";

        case 200:
            return "OK";
        case 201:
            return "Created";
        case 202:
            return "Accepted";
        case 203:
            return "Non-Authoritative Information";
        case 204:
            return "No Content";
        case 205:
            return "Reset Content";
        case 206:
            return "Partial Content";
        case 207:
            return "Multi-Status";
        case 208:
            return "Already Reported";
        case 226:
            return "IM Used";

        case 300:
            return "Multiple Choices";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 303:
            return "See Other";
        case 304:
            return "Not Modified";
        case 305:
            return "Use Proxy";
        case 306:
            return "Switch Proxy";
        case 307:
            return "Temporary Redirect";
        case 308:
            return "Permanent Redirect";

        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 402:
            return "Payment Required";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 406:
            return "Not Acceptable";
        case 407:
            return "Proxy Authentication Required";
        case 408:
            return "Request Timeout";
        case 409:
            return "Conflict";
        case 410:
            return "Gone";
        case 411:
            return "Length Required";
        case 412:
            return "Precondition Failed";
        case 413:
            return "Payload Too Large";
        case 414:
            return "URI Too Long";
        case 415:
            return "Unsupported Media Type";
        case 416:
            return "Range Not Satisfiable";
        case 417:
            return "Expectation Failed";
        case 418:
            return "I'm a teapot";
        case 421:
            return "Misdirected Request";
        case 422:
            return "Unprocessable Entity";
        case 423:
            return "Locked";
        case 424:
            return "Failed Dependency";
        case 425:
            return "Too Early";
        case 426:
            return "Upgrade Required";
        case 428:
            return "Precondition Required";
        case 429:
            return "Too Many Requests";
        case 431:
            return "Request Header Fields Too Large";
        case 451:
            return "Unavailable For Legal Reasons";
        case 499: // nginx's extension to the standard
            return "Client Closed Request";

        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 502:
            return "Bad Gateway";
        case 503:
            return "Service Unavailable";
        case 504:
            return "Gateway Timeout";
        case 505:
            return "HTTP Version Not Supported";
        case 506:
            return "Variant Also Negotiates";
        case 507:
            return "Insufficient Storage";
        case 508:
            return "Loop Detected";
        case 510:
            return "Not Extended";
        case 511:
            return "Network Authentication Required";

        default:
            if(code >= 100 && code < 200)
                return "Informational";

            if(code >= 200 && code < 300)
                return "Successful";

            if(code >= 300 && code < 400)
                return "Redirection";

            if(code >= 400 && code < 500)
                return "Client Error";

            if(code >= 500 && code < 600)
                return "Server Error";

            return "Undefined Error";
    }
}

static inline char *http_header_parse(struct web_client *w, char *s, int parse_useragent) {
    static uint32_t hash_origin = 0, hash_connection = 0, hash_donottrack = 0, hash_useragent = 0,
                    hash_authorization = 0, hash_host = 0, hash_forwarded_host = 0;
    static uint32_t hash_accept_encoding = 0;

    if(unlikely(!hash_origin)) {
        hash_origin = simple_uhash("Origin");
        hash_connection = simple_uhash("Connection");
        hash_accept_encoding = simple_uhash("Accept-Encoding");
        hash_donottrack = simple_uhash("DNT");
        hash_useragent = simple_uhash("User-Agent");
        hash_authorization = simple_uhash("X-Auth-Token");
        hash_host = simple_uhash("Host");
        hash_forwarded_host = simple_uhash("X-Forwarded-Host");
    }

    char *e = s;

    // find the :
    while(*e && *e != ':') e++;
    if(!*e) return e;

    // get the name
    *e = '\0';

    // find the value
    char *v = e + 1, *ve;

    // skip leading spaces from value
    while(*v == ' ') v++;
    ve = v;

    // find the \r
    while(*ve && *ve != '\r') ve++;
    if(!*ve || ve[1] != '\n') {
        *e = ':';
        return ve;
    }

    // terminate the value
    *ve = '\0';

    uint32_t hash = simple_uhash(s);

    if(hash == hash_origin && !strcasecmp(s, "Origin"))
        w->origin = strdupz(v);

    else if(hash == hash_connection && !strcasecmp(s, "Connection")) {
        if(strcasestr(v, "keep-alive"))
            web_client_enable_keepalive(w);
    }
    else if(respect_web_browser_do_not_track_policy && hash == hash_donottrack && !strcasecmp(s, "DNT")) {
        if(*v == '0') web_client_disable_donottrack(w);
        else if(*v == '1') web_client_enable_donottrack(w);
    }
    else if(parse_useragent && hash == hash_useragent && !strcasecmp(s, "User-Agent")) {
        w->user_agent = strdupz(v);
    }
    else if(hash == hash_authorization&& !strcasecmp(s, "X-Auth-Token")) {
        w->auth_bearer_token = strdupz(v);
    }
    else if(hash == hash_host && !strcasecmp(s, "Host")) {
        char buffer[NI_MAXHOST];
        strncpyz(buffer, v, ((size_t)(ve - v) < sizeof(buffer) - 1 ? (size_t)(ve - v) : sizeof(buffer) - 1));
        w->server_host = strdupz(buffer);
    }
    else if(hash == hash_accept_encoding && !strcasecmp(s, "Accept-Encoding")) {
        if(web_enable_gzip) {
            if(strcasestr(v, "gzip"))
                web_client_enable_deflate(w, 1);
            //
            // does not seem to work
            // else if(strcasestr(v, "deflate"))
            //  web_client_enable_deflate(w, 0);
        }
    }
    else if(hash == hash_forwarded_host && !strcasecmp(s, "X-Forwarded-Host")) {
        char buffer[NI_MAXHOST];
        strncpyz(buffer, v, ((size_t)(ve - v) < sizeof(buffer) - 1 ? (size_t)(ve - v) : sizeof(buffer) - 1));
        w->forwarded_host = strdupz(buffer);
    }

    *e = ':';
    *ve = '\r';
    return ve;
}

/**
 * Valid Method
 *
 * Netdata accepts only three methods, including one of these three(STREAM) is an internal method.
 *
 * @param w is the structure with the client request
 * @param s is the start string to parse
 *
 * @return it returns the next address to parse case the method is valid and NULL otherwise.
 */
static inline char *web_client_valid_method(struct web_client *w, char *s) {
    // is is a valid request?
    if(!strncmp(s, "GET ", 4)) {
        s = &s[4];
        w->mode = WEB_CLIENT_MODE_GET;
    }
    else if(!strncmp(s, "OPTIONS ", 8)) {
        s = &s[8];
        w->mode = WEB_CLIENT_MODE_OPTIONS;
    }
    else if(!strncmp(s, "POST ", 5)) {
        s = &s[5];
        w->mode = WEB_CLIENT_MODE_POST;
    }
    else if(!strncmp(s, "PUT ", 4)) {
        s = &s[4];
        w->mode = WEB_CLIENT_MODE_PUT;
    }
    else if(!strncmp(s, "DELETE ", 7)) {
        s = &s[7];
        w->mode = WEB_CLIENT_MODE_DELETE;
    }
    else if(!strncmp(s, "STREAM ", 7)) {
        s = &s[7];

#ifdef ENABLE_HTTPS
        if (!SSL_connection(&w->ssl) && web_client_is_using_ssl_force(w)) {
            w->header_parse_tries = 0;
            w->header_parse_last_size = 0;
            web_client_disable_wait_receive(w);

            char hostname[256];
            char *copyme = strstr(s,"hostname=");
            if ( copyme ){
                copyme += 9;
                char *end = strchr(copyme,'&');
                if(end){
                    size_t length = MIN(255, end - copyme);
                    memcpy(hostname,copyme,length);
                    hostname[length] = 0X00;
                }
                else{
                    memcpy(hostname,"not available",13);
                    hostname[13] = 0x00;
                }
            }
            else{
                memcpy(hostname,"not available",13);
                hostname[13] = 0x00;
            }
            netdata_log_error("The server is configured to always use encrypted connections, please enable the SSL on child with hostname '%s'.",hostname);
            s = NULL;
        }
#endif

        w->mode = WEB_CLIENT_MODE_STREAM;
    }
    else {
        s = NULL;
    }

    return s;
}

/**
 * Request validate
 *
 * @param w is the structure with the client request
 *
 * @return It returns HTTP_VALIDATION_OK on success and another code present
 *          in the enum HTTP_VALIDATION otherwise.
 */
static inline HTTP_VALIDATION http_request_validate(struct web_client *w) {
    char *s = (char *)buffer_tostring(w->response.data), *encoded_url = NULL;

    size_t last_pos = w->header_parse_last_size;

    w->header_parse_tries++;
    w->header_parse_last_size = buffer_strlen(w->response.data);

    int is_it_valid;
    if(w->header_parse_tries > 1) {
        if(last_pos > 4) last_pos -= 4; // allow searching for \r\n\r\n
        else last_pos = 0;

        if(w->header_parse_last_size < last_pos)
            last_pos = 0;

        is_it_valid = url_is_request_complete(s, &s[last_pos], w->header_parse_last_size, &w->post_payload, &w->post_payload_size);
        if(!is_it_valid) {
            if(w->header_parse_tries > HTTP_REQ_MAX_HEADER_FETCH_TRIES) {
                netdata_log_info("Disabling slow client after %zu attempts to read the request (%zu bytes received)", w->header_parse_tries, buffer_strlen(w->response.data));
                w->header_parse_tries = 0;
                w->header_parse_last_size = 0;
                web_client_disable_wait_receive(w);
                return HTTP_VALIDATION_TOO_MANY_READ_RETRIES;
            }

            return HTTP_VALIDATION_INCOMPLETE;
        }

        is_it_valid = 1;
    } else {
        last_pos = w->header_parse_last_size;
        is_it_valid = url_is_request_complete(s, &s[last_pos], w->header_parse_last_size, &w->post_payload, &w->post_payload_size);
    }

    s = web_client_valid_method(w, s);
    if (!s) {
        w->header_parse_tries = 0;
        w->header_parse_last_size = 0;
        web_client_disable_wait_receive(w);

        return HTTP_VALIDATION_NOT_SUPPORTED;
    } else if (!is_it_valid) {
        //Invalid request, we have more data after the end of message
        char *check = strstr((char *)buffer_tostring(w->response.data), "\r\n\r\n");
        if(check) {
            check += 4;
            if (*check) {
                w->header_parse_tries = 0;
                w->header_parse_last_size = 0;
                web_client_disable_wait_receive(w);
                return HTTP_VALIDATION_EXCESS_REQUEST_DATA;
            }
        }
        web_client_enable_wait_receive(w);
        return HTTP_VALIDATION_INCOMPLETE;
    }

    //After the method we have the path and query string together
    encoded_url = s;

    //we search for the position where we have " HTTP/", because it finishes the user request
    s = url_find_protocol(s);

    // incomplete requests
    if(unlikely(!*s)) {
        web_client_enable_wait_receive(w);
        return HTTP_VALIDATION_INCOMPLETE;
    }

    // we have the end of encoded_url - remember it
    char *ue = s;

    // make sure we have complete request
    // complete requests contain: \r\n\r\n
    while(*s) {
        // find a line feed
        while(*s && *s++ != '\r');

        // did we reach the end?
        if(unlikely(!*s)) break;

        // is it \r\n ?
        if(likely(*s++ == '\n')) {

            // is it again \r\n ? (header end)
            if(unlikely(*s == '\r' && s[1] == '\n')) {
                // a valid complete HTTP request found

                char c = *ue;
                *ue = '\0';
                web_client_decode_path_and_query_string(w, encoded_url);
                *ue = c;

#ifdef ENABLE_HTTPS
                if ( (!web_client_check_unix(w)) && (netdata_ssl_web_server_ctx) ) {
                    if (!w->ssl.conn && (web_client_is_using_ssl_force(w) || web_client_is_using_ssl_default(w)) && (w->mode != WEB_CLIENT_MODE_STREAM)) {
                        w->header_parse_tries = 0;
                        w->header_parse_last_size = 0;
                        web_client_disable_wait_receive(w);
                        return HTTP_VALIDATION_REDIRECT;
                    }
                }
#endif

                w->header_parse_tries = 0;
                w->header_parse_last_size = 0;
                web_client_disable_wait_receive(w);
                return HTTP_VALIDATION_OK;
            }

            // another header line
            s = http_header_parse(w, s, (w->mode == WEB_CLIENT_MODE_STREAM)); // parse user agent
        }
    }

    // incomplete request
    web_client_enable_wait_receive(w);
    return HTTP_VALIDATION_INCOMPLETE;
}

static inline ssize_t web_client_send_data(struct web_client *w,const void *buf,size_t len, int flags)
{
    ssize_t bytes;
#ifdef ENABLE_HTTPS
    if ((!web_client_check_unix(w)) && (netdata_ssl_web_server_ctx)) {
        if (SSL_connection(&w->ssl)) {
            bytes = netdata_ssl_write(&w->ssl, buf, len) ;
            web_client_enable_wait_from_ssl(w);
        }
        else
            bytes = send(w->ofd,buf, len , flags);
    } else
        bytes = send(w->ofd,buf, len , flags);
#else
    bytes = send(w->ofd, buf, len, flags);
#endif

    return bytes;
}

void web_client_build_http_header(struct web_client *w) {
    if(unlikely(w->response.code != HTTP_RESP_OK))
        buffer_no_cacheable(w->response.data);

    if(unlikely(!w->response.data->date))
        w->response.data->date = now_realtime_sec();

    // set a proper expiration date, if not already set
    if(unlikely(!w->response.data->expires))
        w->response.data->expires = w->response.data->date +
                ((w->response.data->options & WB_CONTENT_NO_CACHEABLE) ? 0 : 86400);

    // prepare the HTTP response header
    netdata_log_debug(D_WEB_CLIENT, "%llu: Generating HTTP header with response %d.", w->id, w->response.code);

    const char *content_type_string = web_content_type_to_string(w->response.data->content_type);
    const char *code_msg = web_response_code_to_string(w->response.code);

    // prepare the last modified and expiration dates
    char date[32], edate[32];
    {
        struct tm tmbuf, *tm;

        tm = gmtime_r(&w->response.data->date, &tmbuf);
        strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S %Z", tm);

        tm = gmtime_r(&w->response.data->expires, &tmbuf);
        strftime(edate, sizeof(edate), "%a, %d %b %Y %H:%M:%S %Z", tm);
    }

    if (w->response.code == HTTP_RESP_HTTPS_UPGRADE) {
        buffer_sprintf(w->response.header_output,
                       "HTTP/1.1 %d %s\r\n"
                       "Location: https://%s%s\r\n",
                       w->response.code, code_msg,
                       w->server_host ? w->server_host : "",
                       buffer_tostring(w->url_as_received));
        w->response.code = HTTP_RESP_MOVED_PERM;
    }
    else {
        buffer_sprintf(w->response.header_output,
                       "HTTP/1.1 %d %s\r\n"
                       "Connection: %s\r\n"
                       "Server: Netdata Embedded HTTP Server %s\r\n"
                       "Access-Control-Allow-Origin: %s\r\n"
                       "Access-Control-Allow-Credentials: true\r\n"
                       "Content-Type: %s\r\n"
                       "Date: %s\r\n",
                       w->response.code,
                       code_msg,
                       web_client_has_keepalive(w)?"keep-alive":"close",
                       VERSION,
                       w->origin ? w->origin : "*",
                       content_type_string,
                       date);
    }

    if(unlikely(web_x_frame_options))
        buffer_sprintf(w->response.header_output, "X-Frame-Options: %s\r\n", web_x_frame_options);

    if(w->response.has_cookies) {
        if(respect_web_browser_do_not_track_policy)
            buffer_sprintf(w->response.header_output,
                           "Tk: T;cookies\r\n");
    }
    else {
        if(respect_web_browser_do_not_track_policy) {
            if(web_client_has_tracking_required(w))
                buffer_sprintf(w->response.header_output,
                               "Tk: T;cookies\r\n");
            else
                buffer_sprintf(w->response.header_output,
                               "Tk: N\r\n");
        }
    }

    if(w->mode == WEB_CLIENT_MODE_OPTIONS) {
        buffer_strcat(w->response.header_output,
                "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                        "Access-Control-Allow-Headers: accept, x-requested-with, origin, content-type, cookie, pragma, cache-control, x-auth-token\r\n"
                        "Access-Control-Max-Age: 1209600\r\n" // 86400 * 14
        );
    }
    else {
        buffer_sprintf(w->response.header_output,
                "Cache-Control: %s\r\n"
                        "Expires: %s\r\n",
                (w->response.data->options & WB_CONTENT_NO_CACHEABLE)?"no-cache, no-store, must-revalidate\r\nPragma: no-cache":"public",
                edate);
    }

    // copy a possibly available custom header
    if(unlikely(buffer_strlen(w->response.header)))
        buffer_strcat(w->response.header_output, buffer_tostring(w->response.header));

    // headers related to the transfer method
    if(likely(w->response.zoutput))
        buffer_strcat(w->response.header_output, "Content-Encoding: gzip\r\n");

    if(likely(w->flags & WEB_CLIENT_CHUNKED_TRANSFER))
        buffer_strcat(w->response.header_output, "Transfer-Encoding: chunked\r\n");
    else {
        if(likely((w->response.data->len || w->response.rlen))) {
            // we know the content length, put it
            buffer_sprintf(w->response.header_output, "Content-Length: %zu\r\n", w->response.data->len? w->response.data->len: w->response.rlen);
        }
        else {
            // we don't know the content length, disable keep-alive
            web_client_disable_keepalive(w);
        }
    }

    // end of HTTP header
    buffer_strcat(w->response.header_output, "\r\n");
}

static inline void web_client_send_http_header(struct web_client *w) {
    web_client_build_http_header(w);

    // sent the HTTP header
    netdata_log_debug(D_WEB_DATA, "%llu: Sending response HTTP header of size %zu: '%s'"
          , w->id
          , buffer_strlen(w->response.header_output)
          , buffer_tostring(w->response.header_output)
    );

    web_client_cork_socket(w);

    size_t count = 0;
    ssize_t bytes;
#ifdef ENABLE_HTTPS
    if ( (!web_client_check_unix(w)) && (netdata_ssl_web_server_ctx) ) {
        if (SSL_connection(&w->ssl)) {
            bytes = netdata_ssl_write(&w->ssl, buffer_tostring(w->response.header_output), buffer_strlen(w->response.header_output));
            web_client_enable_wait_from_ssl(w);
        }
        else {
            while((bytes = send(w->ofd, buffer_tostring(w->response.header_output), buffer_strlen(w->response.header_output), 0)) == -1) {
                count++;

                if(count > 100 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    netdata_log_error("Cannot send HTTP headers to web client.");
                    break;
                }
            }
        }
    }
    else {
        while((bytes = send(w->ofd, buffer_tostring(w->response.header_output), buffer_strlen(w->response.header_output), 0)) == -1) {
            count++;

            if(count > 100 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                netdata_log_error("Cannot send HTTP headers to web client.");
                break;
            }
        }
    }
#else
    while((bytes = send(w->ofd, buffer_tostring(w->response.header_output), buffer_strlen(w->response.header_output), 0)) == -1) {
        count++;

        if(count > 100 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            netdata_log_error("Cannot send HTTP headers to web client.");
            break;
        }
    }
#endif

    if(bytes != (ssize_t) buffer_strlen(w->response.header_output)) {
        if(bytes > 0)
            w->statistics.sent_bytes += bytes;

        if (bytes < 0) {
            netdata_log_error("HTTP headers failed to be sent (I sent %zu bytes but the system sent %zd bytes). Closing web client."
                  , buffer_strlen(w->response.header_output)
                  , bytes);

            WEB_CLIENT_IS_DEAD(w);
            return;
        }
    }
    else
        w->statistics.sent_bytes += bytes;
}

static inline int web_client_switch_host(RRDHOST *host, struct web_client *w, char *url, bool nodeid, int (*func)(RRDHOST *, struct web_client *, char *)) {
    static uint32_t hash_localhost = 0;

    if(unlikely(!hash_localhost)) {
        hash_localhost = simple_hash("localhost");
    }

    if(host != localhost) {
        buffer_flush(w->response.data);
        buffer_strcat(w->response.data, "Nesting of hosts is not allowed.");
        return HTTP_RESP_BAD_REQUEST;
    }

    char *tok = strsep_skip_consecutive_separators(&url, "/");
    if(tok && *tok) {
        netdata_log_debug(D_WEB_CLIENT, "%llu: Searching for host with name '%s'.", w->id, tok);

        if(nodeid) {
            host = find_host_by_node_id(tok);
            if(!host) {
                host = rrdhost_find_by_hostname(tok);
                if (!host)
                    host = rrdhost_find_by_guid(tok);
            }
        }
        else {
            host = rrdhost_find_by_hostname(tok);
            if(!host) {
                host = rrdhost_find_by_guid(tok);
                if (!host)
                    host = find_host_by_node_id(tok);
            }
        }

        if(!host) {
            // we didn't find it, but it may be a uuid case mismatch for MACHINE_GUID
            // so, recreate the machine guid in lower-case.
            uuid_t uuid;
            char txt[UUID_STR_LEN];
            if (uuid_parse(tok, uuid) == 0) {
                uuid_unparse_lower(uuid, txt);
                host = rrdhost_find_by_guid(txt);
            }
        }

        if (host) {
            if(!url)
                //no delim found
                return append_slash_to_url_and_redirect(w);

            size_t len = strlen(url) + 2;
            char buf[len];
            buf[0] = '/';
            strcpy(&buf[1], url);
            buf[len - 1] = '\0';

            buffer_flush(w->url_path_decoded);
            buffer_strcat(w->url_path_decoded, buf);
            return func(host, w, buf);
        }
    }

    buffer_flush(w->response.data);
    w->response.data->content_type = CT_TEXT_HTML;
    buffer_strcat(w->response.data, "This netdata does not maintain a database for host: ");
    buffer_strcat_htmlescape(w->response.data, tok?tok:"");
    return HTTP_RESP_NOT_FOUND;
}

int web_client_api_request_with_node_selection(RRDHOST *host, struct web_client *w, char *decoded_url_path) {
    static uint32_t
            hash_api = 0,
            hash_host = 0,
            hash_node = 0;

    if(unlikely(!hash_api)) {
        hash_api = simple_hash("api");
        hash_host = simple_hash("host");
        hash_node = simple_hash("node");
    }

    char *tok = strsep_skip_consecutive_separators(&decoded_url_path, "/?");
    if(likely(tok && *tok)) {
        uint32_t hash = simple_hash(tok);

        if(unlikely(hash == hash_api && strcmp(tok, "api") == 0)) {
            // current API
            netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: API request ...", w->id);
            return check_host_and_call(host, w, decoded_url_path, web_client_api_request);
        }
        else if(unlikely((hash == hash_host && strcmp(tok, "host") == 0) || (hash == hash_node && strcmp(tok, "node") == 0))) {
            // host switching
            netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: host switch request ...", w->id);
            return web_client_switch_host(host, w, decoded_url_path, hash == hash_node, web_client_api_request_with_node_selection);
        }
    }

    buffer_flush(w->response.data);
    buffer_strcat(w->response.data, "Unknown API endpoint.");
    w->response.data->content_type = CT_TEXT_HTML;
    return HTTP_RESP_NOT_FOUND;
}

static inline int web_client_process_url(RRDHOST *host, struct web_client *w, char *decoded_url_path) {
    if(unlikely(!service_running(ABILITY_WEB_REQUESTS)))
        return web_client_permission_denied(w);

    static uint32_t
            hash_api = 0,
            hash_netdata_conf = 0,
            hash_host = 0,
            hash_node = 0,
            hash_v0 = 0,
            hash_v1 = 0,
            hash_v2 = 0;

#ifdef NETDATA_INTERNAL_CHECKS
    static uint32_t hash_exit = 0, hash_debug = 0, hash_mirror = 0;
#endif

    if(unlikely(!hash_api)) {
        hash_api = simple_hash("api");
        hash_netdata_conf = simple_hash("netdata.conf");
        hash_host = simple_hash("host");
        hash_node = simple_hash("node");
        hash_v0 = simple_hash("v0");
        hash_v1 = simple_hash("v1");
        hash_v2 = simple_hash("v2");
#ifdef NETDATA_INTERNAL_CHECKS
        hash_exit = simple_hash("exit");
        hash_debug = simple_hash("debug");
        hash_mirror = simple_hash("mirror");
#endif
    }

    // keep a copy of the decoded path, in case we need to serve it as a filename
    char filename[FILENAME_MAX + 1];
    strncpyz(filename, decoded_url_path ? decoded_url_path : "", FILENAME_MAX);

    char *tok = strsep_skip_consecutive_separators(&decoded_url_path, "/?");
    if(likely(tok && *tok)) {
        uint32_t hash = simple_hash(tok);
        netdata_log_debug(D_WEB_CLIENT, "%llu: Processing command '%s'.", w->id, tok);

        if(likely(hash == hash_api && strcmp(tok, "api") == 0)) {                           // current API
            netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: API request ...", w->id);
            return check_host_and_call(host, w, decoded_url_path, web_client_api_request);
        }
        else if(unlikely((hash == hash_host && strcmp(tok, "host") == 0) || (hash == hash_node && strcmp(tok, "node") == 0))) { // host switching
            netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: host switch request ...", w->id);
            return web_client_switch_host(host, w, decoded_url_path, hash == hash_node, web_client_process_url);
        }
        else if(unlikely(hash == hash_v2 && strcmp(tok, "v2") == 0)) {
            if(web_client_flag_check(w, WEB_CLIENT_FLAG_PATH_WITH_VERSION))
                return bad_request_multiple_dashboard_versions(w);
            web_client_flag_set(w, WEB_CLIENT_FLAG_PATH_IS_V2);
            return web_client_process_url(host, w, decoded_url_path);
        }
        else if(unlikely(hash == hash_v1 && strcmp(tok, "v1") == 0)) {
            if(web_client_flag_check(w, WEB_CLIENT_FLAG_PATH_WITH_VERSION))
                return bad_request_multiple_dashboard_versions(w);
            web_client_flag_set(w, WEB_CLIENT_FLAG_PATH_IS_V1);
            return web_client_process_url(host, w, decoded_url_path);
        }
        else if(unlikely(hash == hash_v0 && strcmp(tok, "v0") == 0)) {
            if(web_client_flag_check(w, WEB_CLIENT_FLAG_PATH_WITH_VERSION))
                return bad_request_multiple_dashboard_versions(w);
            web_client_flag_set(w, WEB_CLIENT_FLAG_PATH_IS_V0);
            return web_client_process_url(host, w, decoded_url_path);
        }
        else if(unlikely(hash == hash_netdata_conf && strcmp(tok, "netdata.conf") == 0)) {    // netdata.conf
            if(unlikely(!web_client_can_access_netdataconf(w)))
                return web_client_permission_denied(w);

            netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: generating netdata.conf ...", w->id);
            w->response.data->content_type = CT_TEXT_PLAIN;
            buffer_flush(w->response.data);
            config_generate(w->response.data, 0);
            return HTTP_RESP_OK;
        }
#ifdef NETDATA_INTERNAL_CHECKS
        else if(unlikely(hash == hash_exit && strcmp(tok, "exit") == 0)) {
            if(unlikely(!web_client_can_access_netdataconf(w)))
                return web_client_permission_denied(w);

            w->response.data->content_type = CT_TEXT_PLAIN;
            buffer_flush(w->response.data);

            if(!netdata_exit)
                buffer_strcat(w->response.data, "ok, will do...");
            else
                buffer_strcat(w->response.data, "I am doing it already");

            netdata_log_error("web request to exit received.");
            netdata_cleanup_and_exit(0);
            return HTTP_RESP_OK;
        }
        else if(unlikely(hash == hash_debug && strcmp(tok, "debug") == 0)) {
            if(unlikely(!web_client_can_access_netdataconf(w)))
                return web_client_permission_denied(w);

            buffer_flush(w->response.data);

            // get the name of the data to show
            tok = strsep_skip_consecutive_separators(&decoded_url_path, "&");
            if(tok && *tok) {
                netdata_log_debug(D_WEB_CLIENT, "%llu: Searching for RRD data with name '%s'.", w->id, tok);

                // do we have such a data set?
                RRDSET *st = rrdset_find_byname(host, tok);
                if(!st) st = rrdset_find(host, tok);
                if(!st) {
                    w->response.data->content_type = CT_TEXT_HTML;
                    buffer_strcat(w->response.data, "Chart is not found: ");
                    buffer_strcat_htmlescape(w->response.data, tok);
                    netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: %s is not found.", w->id, tok);
                    return HTTP_RESP_NOT_FOUND;
                }

                debug_flags |= D_RRD_STATS;

                if(rrdset_flag_check(st, RRDSET_FLAG_DEBUG))
                    rrdset_flag_clear(st, RRDSET_FLAG_DEBUG);
                else
                    rrdset_flag_set(st, RRDSET_FLAG_DEBUG);

                w->response.data->content_type = CT_TEXT_HTML;
                buffer_sprintf(w->response.data, "Chart has now debug %s: ", rrdset_flag_check(st, RRDSET_FLAG_DEBUG)?"enabled":"disabled");
                buffer_strcat_htmlescape(w->response.data, tok);
                netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: debug for %s is %s.", w->id, tok, rrdset_flag_check(st, RRDSET_FLAG_DEBUG)?"enabled":"disabled");
                return HTTP_RESP_OK;
            }

            buffer_flush(w->response.data);
            buffer_strcat(w->response.data, "debug which chart?\r\n");
            return HTTP_RESP_BAD_REQUEST;
        }
        else if(unlikely(hash == hash_mirror && strcmp(tok, "mirror") == 0)) {
            if(unlikely(!web_client_can_access_netdataconf(w)))
                return web_client_permission_denied(w);

            netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: Mirroring...", w->id);

            // replace the zero bytes with spaces
            buffer_char_replace(w->response.data, '\0', ' ');

            // just leave the buffer as-is
            // it will be copied back to the client

            return HTTP_RESP_OK;
        }
#endif  /* NETDATA_INTERNAL_CHECKS */
    }

    buffer_flush(w->response.data);
    return mysendfile(w, filename);
}

void web_client_process_request(struct web_client *w) {

    // start timing us
    web_client_timeout_checkpoint_init(w);

    switch(http_request_validate(w)) {
        case HTTP_VALIDATION_OK:
            switch(w->mode) {
                case WEB_CLIENT_MODE_STREAM:
                    if(unlikely(!web_client_can_access_stream(w))) {
                        web_client_permission_denied(w);
                        return;
                    }

                    w->response.code = rrdpush_receiver_thread_spawn(w, (char *)buffer_tostring(w->url_query_string_decoded));
                    return;

                case WEB_CLIENT_MODE_OPTIONS:
                    if(unlikely(
                            !web_client_can_access_dashboard(w) &&
                            !web_client_can_access_registry(w) &&
                            !web_client_can_access_badges(w) &&
                            !web_client_can_access_mgmt(w) &&
                            !web_client_can_access_netdataconf(w)
                    )) {
                        web_client_permission_denied(w);
                        break;
                    }

                    w->response.data->content_type = CT_TEXT_PLAIN;
                    buffer_flush(w->response.data);
                    buffer_strcat(w->response.data, "OK");
                    w->response.code = HTTP_RESP_OK;
                    break;

                case WEB_CLIENT_MODE_FILECOPY:
                case WEB_CLIENT_MODE_POST:
                case WEB_CLIENT_MODE_GET:
                case WEB_CLIENT_MODE_PUT:
                case WEB_CLIENT_MODE_DELETE:
                    if(unlikely(
                            !web_client_can_access_dashboard(w) &&
                            !web_client_can_access_registry(w) &&
                            !web_client_can_access_badges(w) &&
                            !web_client_can_access_mgmt(w) &&
                            !web_client_can_access_netdataconf(w)
                    )) {
                        web_client_permission_denied(w);
                        break;
                    }

                    web_client_reset_path_flags(w);

                    // find if the URL path has a filename extension
                    char path[FILENAME_MAX + 1];
                    strncpyz(path, buffer_tostring(w->url_path_decoded), FILENAME_MAX);
                    char *s = path, *e = path;

                    // remove the query string and find the last char
                    for (; *e ; e++) {
                        if (*e == '?')
                            break;
                    }

                    if(e == s || (*(e - 1) == '/'))
                        web_client_flag_set(w, WEB_CLIENT_FLAG_PATH_HAS_TRAILING_SLASH);

                    // check if there is a filename extension
                    while (--e > s) {
                        if (*e == '/')
                            break;
                        if(*e == '.') {
                            web_client_flag_set(w, WEB_CLIENT_FLAG_PATH_HAS_FILE_EXTENSION);
                            break;
                        }
                    }

                    w->response.code = (short)web_client_process_url(localhost, w, path);
                    break;
            }
            break;

        case HTTP_VALIDATION_INCOMPLETE:
            if(w->response.data->len > NETDATA_WEB_REQUEST_MAX_SIZE) {
                buffer_flush(w->url_as_received);
                buffer_strcat(w->url_as_received, "too big request");

                netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: Received request is too big (%zu bytes).", w->id, w->response.data->len);

                size_t len = w->response.data->len;
                buffer_flush(w->response.data);
                buffer_sprintf(w->response.data, "Received request is too big  (received %zu bytes, max is %zu bytes).\r\n", len, (size_t)NETDATA_WEB_REQUEST_MAX_SIZE);
                w->response.code = HTTP_RESP_BAD_REQUEST;
            }
            else {
                // wait for more data
                // set to normal to prevent web_server_rcv_callback
                // from going into stream mode
                if (w->mode == WEB_CLIENT_MODE_STREAM)
                    w->mode = WEB_CLIENT_MODE_GET;
                return;
            }
            break;
#ifdef ENABLE_HTTPS
        case HTTP_VALIDATION_REDIRECT:
        {
            buffer_flush(w->response.data);
            w->response.data->content_type = CT_TEXT_HTML;
            buffer_strcat(w->response.data,
                          "<!DOCTYPE html><!-- SPDX-License-Identifier: GPL-3.0-or-later --><html>"
                          "<body onload=\"window.location.href ='https://'+ window.location.hostname +"
                          " ':' + window.location.port + window.location.pathname + window.location.search\">"
                          "Redirecting to safety connection, case your browser does not support redirection, please"
                          " click <a onclick=\"window.location.href ='https://'+ window.location.hostname + ':' "
                          " + window.location.port + window.location.pathname + window.location.search\">here</a>."
                          "</body></html>");
            w->response.code = HTTP_RESP_HTTPS_UPGRADE;
            break;
        }
#endif
        case HTTP_VALIDATION_MALFORMED_URL:
            netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: Malformed URL '%s'.", w->id, w->response.data->buffer);

            buffer_flush(w->response.data);
            buffer_strcat(w->response.data, "Malformed URL...\r\n");
            w->response.code = HTTP_RESP_BAD_REQUEST;
            break;
        case HTTP_VALIDATION_EXCESS_REQUEST_DATA:
            netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: Excess data in request '%s'.", w->id, w->response.data->buffer);

            buffer_flush(w->response.data);
            buffer_strcat(w->response.data, "Excess data in request.\r\n");
            w->response.code = HTTP_RESP_BAD_REQUEST;
            break;
        case HTTP_VALIDATION_TOO_MANY_READ_RETRIES:
            netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: Too many retries to read request '%s'.", w->id, w->response.data->buffer);

            buffer_flush(w->response.data);
            buffer_strcat(w->response.data, "Too many retries to read request.\r\n");
            w->response.code = HTTP_RESP_BAD_REQUEST;
            break;
        case HTTP_VALIDATION_NOT_SUPPORTED:
            netdata_log_debug(D_WEB_CLIENT_ACCESS, "%llu: HTTP method requested is not supported '%s'.", w->id, w->response.data->buffer);

            buffer_flush(w->response.data);
            buffer_strcat(w->response.data, "HTTP method requested is not supported...\r\n");
            w->response.code = HTTP_RESP_BAD_REQUEST;
            break;
    }

    // keep track of the processing time
    web_client_timeout_checkpoint_response_ready(w, NULL);

    w->response.sent = 0;

    web_client_send_http_header(w);

    // enable sending immediately if we have data
    if(w->response.data->len) web_client_enable_wait_send(w);
    else web_client_disable_wait_send(w);

    switch(w->mode) {
        case WEB_CLIENT_MODE_STREAM:
            netdata_log_debug(D_WEB_CLIENT, "%llu: STREAM done.", w->id);
            break;

        case WEB_CLIENT_MODE_OPTIONS:
            netdata_log_debug(D_WEB_CLIENT, "%llu: Done preparing the OPTIONS response. Sending data (%zu bytes) to client.", w->id, w->response.data->len);
            break;

        case WEB_CLIENT_MODE_POST:
        case WEB_CLIENT_MODE_GET:
        case WEB_CLIENT_MODE_PUT:
        case WEB_CLIENT_MODE_DELETE:
            netdata_log_debug(D_WEB_CLIENT, "%llu: Done preparing the response. Sending data (%zu bytes) to client.", w->id, w->response.data->len);
            break;

        case WEB_CLIENT_MODE_FILECOPY:
            if(w->response.rlen) {
                netdata_log_debug(D_WEB_CLIENT, "%llu: Done preparing the response. Will be sending data file of %zu bytes to client.", w->id, w->response.rlen);
                web_client_enable_wait_receive(w);

                /*
                // utilize the kernel sendfile() for copying the file to the socket.
                // this block of code can be commented, without anything missing.
                // when it is commented, the program will copy the data using async I/O.
                {
                    long len = sendfile(w->ofd, w->ifd, NULL, w->response.data->rbytes);
                    if(len != w->response.data->rbytes)
                        netdata_log_error("%llu: sendfile() should copy %ld bytes, but copied %ld. Falling back to manual copy.", w->id, w->response.data->rbytes, len);
                    else
                        web_client_request_done(w);
                }
                */
            }
            else
                netdata_log_debug(D_WEB_CLIENT, "%llu: Done preparing the response. Will be sending an unknown amount of bytes to client.", w->id);
            break;

        default:
            fatal("%llu: Unknown client mode %u.", w->id, w->mode);
            break;
    }
}

ssize_t web_client_send_chunk_header(struct web_client *w, size_t len)
{
    netdata_log_debug(D_DEFLATE, "%llu: OPEN CHUNK of %zu bytes (hex: %zx).", w->id, len, len);
    char buf[24];
    ssize_t bytes;
    bytes = (ssize_t)sprintf(buf, "%zX\r\n", len);
    buf[bytes] = 0x00;

    bytes = web_client_send_data(w,buf,strlen(buf),0);
    if(bytes > 0) {
        netdata_log_debug(D_DEFLATE, "%llu: Sent chunk header %zd bytes.", w->id, bytes);
        w->statistics.sent_bytes += bytes;
    }

    else if(bytes == 0) {
        netdata_log_debug(D_WEB_CLIENT, "%llu: Did not send chunk header to the client.", w->id);
    }
    else {
        netdata_log_debug(D_WEB_CLIENT, "%llu: Failed to send chunk header to client.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }

    return bytes;
}

ssize_t web_client_send_chunk_close(struct web_client *w)
{
    //debug(D_DEFLATE, "%llu: CLOSE CHUNK.", w->id);

    ssize_t bytes;
    bytes = web_client_send_data(w,"\r\n",2,0);
    if(bytes > 0) {
        netdata_log_debug(D_DEFLATE, "%llu: Sent chunk suffix %zd bytes.", w->id, bytes);
        w->statistics.sent_bytes += bytes;
    }

    else if(bytes == 0) {
        netdata_log_debug(D_WEB_CLIENT, "%llu: Did not send chunk suffix to the client.", w->id);
    }
    else {
        netdata_log_debug(D_WEB_CLIENT, "%llu: Failed to send chunk suffix to client.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }

    return bytes;
}

ssize_t web_client_send_chunk_finalize(struct web_client *w)
{
    //debug(D_DEFLATE, "%llu: FINALIZE CHUNK.", w->id);

    ssize_t bytes;
    bytes = web_client_send_data(w,"\r\n0\r\n\r\n",7,0);
    if(bytes > 0) {
        netdata_log_debug(D_DEFLATE, "%llu: Sent chunk suffix %zd bytes.", w->id, bytes);
        w->statistics.sent_bytes += bytes;
    }

    else if(bytes == 0) {
        netdata_log_debug(D_WEB_CLIENT, "%llu: Did not send chunk finalize suffix to the client.", w->id);
    }
    else {
        netdata_log_debug(D_WEB_CLIENT, "%llu: Failed to send chunk finalize suffix to client.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }

    return bytes;
}

ssize_t web_client_send_deflate(struct web_client *w)
{
    ssize_t len = 0, t = 0;

    // when using compression,
    // w->response.sent is the amount of bytes passed through compression

    netdata_log_debug(D_DEFLATE, "%llu: web_client_send_deflate(): w->response.data->len = %zu, w->response.sent = %zu, w->response.zhave = %zu, w->response.zsent = %zu, w->response.zstream.avail_in = %u, w->response.zstream.avail_out = %u, w->response.zstream.total_in = %lu, w->response.zstream.total_out = %lu.",
        w->id, w->response.data->len, w->response.sent, w->response.zhave, w->response.zsent, w->response.zstream.avail_in, w->response.zstream.avail_out, w->response.zstream.total_in, w->response.zstream.total_out);

    if(w->response.data->len - w->response.sent == 0 && w->response.zstream.avail_in == 0 && w->response.zhave == w->response.zsent && w->response.zstream.avail_out != 0) {
        // there is nothing to send

        netdata_log_debug(D_WEB_CLIENT, "%llu: Out of output data.", w->id);

        // finalize the chunk
        if(w->response.sent != 0) {
            t = web_client_send_chunk_finalize(w);
            if(t < 0) return t;
        }

        if(w->mode == WEB_CLIENT_MODE_FILECOPY && web_client_has_wait_receive(w) && w->response.rlen && w->response.rlen > w->response.data->len) {
            // we have to wait, more data will come
            netdata_log_debug(D_WEB_CLIENT, "%llu: Waiting for more data to become available.", w->id);
            web_client_disable_wait_send(w);
            return t;
        }

        if(unlikely(!web_client_has_keepalive(w))) {
            netdata_log_debug(D_WEB_CLIENT, "%llu: Closing (keep-alive is not enabled). %zu bytes sent.", w->id, w->response.sent);
            WEB_CLIENT_IS_DEAD(w);
            return t;
        }

        // reset the client
        web_client_request_done(w);
        netdata_log_debug(D_WEB_CLIENT, "%llu: Done sending all data on socket.", w->id);
        return t;
    }

    if(w->response.zhave == w->response.zsent) {
        // compress more input data

        // close the previous open chunk
        if(w->response.sent != 0) {
            t = web_client_send_chunk_close(w);
            if(t < 0) return t;
        }

        netdata_log_debug(D_DEFLATE, "%llu: Compressing %zu new bytes starting from %zu (and %u left behind).", w->id, (w->response.data->len - w->response.sent), w->response.sent, w->response.zstream.avail_in);

        // give the compressor all the data not passed through the compressor yet
        if(w->response.data->len > w->response.sent) {
            w->response.zstream.next_in = (Bytef *)&w->response.data->buffer[w->response.sent - w->response.zstream.avail_in];
            w->response.zstream.avail_in += (uInt) (w->response.data->len - w->response.sent);
        }

        // reset the compressor output buffer
        w->response.zstream.next_out = w->response.zbuffer;
        w->response.zstream.avail_out = NETDATA_WEB_RESPONSE_ZLIB_CHUNK_SIZE;

        // ask for FINISH if we have all the input
        int flush = Z_SYNC_FLUSH;
        if((w->mode == WEB_CLIENT_MODE_GET || w->mode == WEB_CLIENT_MODE_POST || w->mode == WEB_CLIENT_MODE_PUT || w->mode == WEB_CLIENT_MODE_DELETE)
            || (w->mode == WEB_CLIENT_MODE_FILECOPY && !web_client_has_wait_receive(w) && w->response.data->len == w->response.rlen)) {
            flush = Z_FINISH;
            netdata_log_debug(D_DEFLATE, "%llu: Requesting Z_FINISH, if possible.", w->id);
        }
        else {
            netdata_log_debug(D_DEFLATE, "%llu: Requesting Z_SYNC_FLUSH.", w->id);
        }

        // compress
        if(deflate(&w->response.zstream, flush) == Z_STREAM_ERROR) {
            netdata_log_error("%llu: Compression failed. Closing down client.", w->id);
            web_client_request_done(w);
            return(-1);
        }

        w->response.zhave = NETDATA_WEB_RESPONSE_ZLIB_CHUNK_SIZE - w->response.zstream.avail_out;
        w->response.zsent = 0;

        // keep track of the bytes passed through the compressor
        w->response.sent = w->response.data->len;

        netdata_log_debug(D_DEFLATE, "%llu: Compression produced %zu bytes.", w->id, w->response.zhave);

        // open a new chunk
        ssize_t t2 = web_client_send_chunk_header(w, w->response.zhave);
        if(t2 < 0) return t2;
        t += t2;
    }

    netdata_log_debug(D_WEB_CLIENT, "%llu: Sending %zu bytes of data (+%zd of chunk header).", w->id, w->response.zhave - w->response.zsent, t);

    len = web_client_send_data(w,&w->response.zbuffer[w->response.zsent], (size_t) (w->response.zhave - w->response.zsent), MSG_DONTWAIT);
    if(len > 0) {
        w->statistics.sent_bytes += len;
        w->response.zsent += len;
        len += t;
        netdata_log_debug(D_WEB_CLIENT, "%llu: Sent %zd bytes.", w->id, len);
    }
    else if(len == 0) {
        netdata_log_debug(D_WEB_CLIENT, "%llu: Did not send any bytes to the client (zhave = %zu, zsent = %zu, need to send = %zu).",
            w->id, w->response.zhave, w->response.zsent, w->response.zhave - w->response.zsent);

    }
    else {
        netdata_log_debug(D_WEB_CLIENT, "%llu: Failed to send data to client.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }

    return(len);
}

ssize_t web_client_send(struct web_client *w) {
    if(likely(w->response.zoutput)) return web_client_send_deflate(w);

    ssize_t bytes;

    if(unlikely(w->response.data->len - w->response.sent == 0)) {
        // there is nothing to send

        netdata_log_debug(D_WEB_CLIENT, "%llu: Out of output data.", w->id);

        // there can be two cases for this
        // A. we have done everything
        // B. we temporarily have nothing to send, waiting for the buffer to be filled by ifd

        if(w->mode == WEB_CLIENT_MODE_FILECOPY && web_client_has_wait_receive(w) && w->response.rlen && w->response.rlen > w->response.data->len) {
            // we have to wait, more data will come
            netdata_log_debug(D_WEB_CLIENT, "%llu: Waiting for more data to become available.", w->id);
            web_client_disable_wait_send(w);
            return 0;
        }

        if(unlikely(!web_client_has_keepalive(w))) {
            netdata_log_debug(D_WEB_CLIENT, "%llu: Closing (keep-alive is not enabled). %zu bytes sent.", w->id, w->response.sent);
            WEB_CLIENT_IS_DEAD(w);
            return 0;
        }

        web_client_request_done(w);
        netdata_log_debug(D_WEB_CLIENT, "%llu: Done sending all data on socket. Waiting for next request on the same socket.", w->id);
        return 0;
    }

    bytes = web_client_send_data(w,&w->response.data->buffer[w->response.sent], w->response.data->len - w->response.sent, MSG_DONTWAIT);
    if(likely(bytes > 0)) {
        w->statistics.sent_bytes += bytes;
        w->response.sent += bytes;
        netdata_log_debug(D_WEB_CLIENT, "%llu: Sent %zd bytes.", w->id, bytes);
    }
    else if(likely(bytes == 0)) {
        netdata_log_debug(D_WEB_CLIENT, "%llu: Did not send any bytes to the client.", w->id);
    }
    else {
        netdata_log_debug(D_WEB_CLIENT, "%llu: Failed to send data to client.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }

    return(bytes);
}

ssize_t web_client_read_file(struct web_client *w)
{
    if(unlikely(w->response.rlen > w->response.data->size))
        buffer_need_bytes(w->response.data, w->response.rlen - w->response.data->size);

    if(unlikely(w->response.rlen <= w->response.data->len))
        return 0;

    ssize_t left = (ssize_t)(w->response.rlen - w->response.data->len);
    ssize_t bytes = read(w->ifd, &w->response.data->buffer[w->response.data->len], (size_t)left);
    if(likely(bytes > 0)) {
        size_t old = w->response.data->len;
        (void)old;

        w->response.data->len += bytes;
        w->response.data->buffer[w->response.data->len] = '\0';

        netdata_log_debug(D_WEB_CLIENT, "%llu: Read %zd bytes.", w->id, bytes);
        netdata_log_debug(D_WEB_DATA, "%llu: Read data: '%s'.", w->id, &w->response.data->buffer[old]);

        web_client_enable_wait_send(w);

        if(w->response.rlen && w->response.data->len >= w->response.rlen)
            web_client_disable_wait_receive(w);
    }
    else if(likely(bytes == 0)) {
        netdata_log_debug(D_WEB_CLIENT, "%llu: Out of input file data.", w->id);

        // if we cannot read, it means we have an error on input.
        // if however, we are copying a file from ifd to ofd, we should not return an error.
        // in this case, the error should be generated when the file has been sent to the client.

        // we are copying data from ifd to ofd
        // let it finish copying...
        web_client_disable_wait_receive(w);

        netdata_log_debug(D_WEB_CLIENT, "%llu: Read the whole file.", w->id);

        if(web_server_mode != WEB_SERVER_MODE_STATIC_THREADED) {
            if (w->ifd != w->ofd) close(w->ifd);
        }

        w->ifd = w->ofd;
    }
    else {
        netdata_log_debug(D_WEB_CLIENT, "%llu: read data failed.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }

    return(bytes);
}

ssize_t web_client_receive(struct web_client *w)
{
    if(unlikely(w->mode == WEB_CLIENT_MODE_FILECOPY))
        return web_client_read_file(w);

    ssize_t bytes;
    ssize_t left = (ssize_t)(w->response.data->size - w->response.data->len);

    // do we have any space for more data?
    buffer_need_bytes(w->response.data, NETDATA_WEB_REQUEST_INITIAL_SIZE);

    errno = 0;

#ifdef ENABLE_HTTPS
    if ( (!web_client_check_unix(w)) && (netdata_ssl_web_server_ctx) ) {
        if (SSL_connection(&w->ssl)) {
            bytes = netdata_ssl_read(&w->ssl, &w->response.data->buffer[w->response.data->len], (size_t) (left - 1));
            web_client_enable_wait_from_ssl(w);
        }
        else {
            bytes = recv(w->ifd, &w->response.data->buffer[w->response.data->len], (size_t) (left - 1), MSG_DONTWAIT);
        }
    }
    else{
        bytes = recv(w->ifd, &w->response.data->buffer[w->response.data->len], (size_t) (left - 1), MSG_DONTWAIT);
    }
#else
    bytes = recv(w->ifd, &w->response.data->buffer[w->response.data->len], (size_t) (left - 1), MSG_DONTWAIT);
#endif

    if(likely(bytes > 0)) {
        w->statistics.received_bytes += bytes;

        size_t old = w->response.data->len;
        (void)old;

        w->response.data->len += bytes;
        w->response.data->buffer[w->response.data->len] = '\0';

        netdata_log_debug(D_WEB_CLIENT, "%llu: Received %zd bytes.", w->id, bytes);
        netdata_log_debug(D_WEB_DATA, "%llu: Received data: '%s'.", w->id, &w->response.data->buffer[old]);
    }
    else if(unlikely(bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
        web_client_enable_wait_receive(w);
        return 0;
    }
    else if (bytes < 0) {
        netdata_log_debug(D_WEB_CLIENT, "%llu: receive data failed.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    } else
        netdata_log_debug(D_WEB_CLIENT, "%llu: Received %zd bytes.", w->id, bytes);

    return(bytes);
}

void web_client_decode_path_and_query_string(struct web_client *w, const char *path_and_query_string) {
    char buffer[NETDATA_WEB_REQUEST_URL_SIZE + 2];
    buffer[0] = '\0';

    buffer_flush(w->url_path_decoded);
    buffer_flush(w->url_query_string_decoded);

    if(buffer_strlen(w->url_as_received) == 0)
        // do not overwrite this if it is already filled
        buffer_strcat(w->url_as_received, path_and_query_string);

    if(w->mode == WEB_CLIENT_MODE_STREAM) {
        // in stream mode, there is no path

        url_decode_r(buffer, path_and_query_string, NETDATA_WEB_REQUEST_URL_SIZE + 1);

        buffer[NETDATA_WEB_REQUEST_URL_SIZE + 1] = '\0';
        buffer_strcat(w->url_query_string_decoded, buffer);
    }
    else {
        // in non-stream mode, there is a path
        // FIXME - the way this is implemented, query string params never accept the symbol &, not even encoded as %26
        // To support the symbol & in query string params, we need to turn the url_query_string_decoded into a
        // dictionary and decode each of the parameters individually.
        // OR: in url_query_string_decoded use as separator a control character that cannot appear in the URL.

        url_decode_r(buffer, path_and_query_string, NETDATA_WEB_REQUEST_URL_SIZE + 1);

        char *question_mark_start = strchr(buffer, '?');
        if (question_mark_start) {
            buffer_strcat(w->url_query_string_decoded, question_mark_start);
            char c = *question_mark_start;
            *question_mark_start = '\0';
            buffer_strcat(w->url_path_decoded, buffer);
            *question_mark_start = c;
        } else {
            buffer_strcat(w->url_query_string_decoded, "");
            buffer_strcat(w->url_path_decoded, buffer);
        }
    }
}

void web_client_reuse_from_cache(struct web_client *w) {
    // zero everything about it - but keep the buffers

    web_client_reset_allocations(w, false);

    // remember the pointers to the buffers
    BUFFER *b1 = w->response.data;
    BUFFER *b2 = w->response.header;
    BUFFER *b3 = w->response.header_output;
    BUFFER *b4 = w->url_path_decoded;
    BUFFER *b5 = w->url_as_received;
    BUFFER *b6 = w->url_query_string_decoded;

#ifdef ENABLE_HTTPS
    NETDATA_SSL ssl = w->ssl;
#endif

    size_t use_count = w->use_count;
    size_t *statistics_memory_accounting = w->statistics.memory_accounting;

    // zero everything
    memset(w, 0, sizeof(struct web_client));

    w->ifd = w->ofd = -1;
    w->statistics.memory_accounting = statistics_memory_accounting;
    w->use_count = use_count;

#ifdef ENABLE_HTTPS
    w->ssl = ssl;
#endif

    // restore the pointers of the buffers
    w->response.data = b1;
    w->response.header = b2;
    w->response.header_output = b3;
    w->url_path_decoded = b4;
    w->url_as_received = b5;
    w->url_query_string_decoded = b6;
}

struct web_client *web_client_create(size_t *statistics_memory_accounting) {
    struct web_client *w = (struct web_client *)callocz(1, sizeof(struct web_client));

#ifdef ENABLE_HTTPS
    w->ssl = NETDATA_SSL_UNSET_CONNECTION;
#endif

    w->use_count = 1;
    w->statistics.memory_accounting = statistics_memory_accounting;

    w->url_as_received = buffer_create(NETDATA_WEB_DECODED_URL_INITIAL_SIZE, w->statistics.memory_accounting);
    w->url_path_decoded = buffer_create(NETDATA_WEB_DECODED_URL_INITIAL_SIZE, w->statistics.memory_accounting);
    w->url_query_string_decoded = buffer_create(NETDATA_WEB_DECODED_URL_INITIAL_SIZE, w->statistics.memory_accounting);
    w->response.data = buffer_create(NETDATA_WEB_RESPONSE_INITIAL_SIZE, w->statistics.memory_accounting);
    w->response.header = buffer_create(NETDATA_WEB_RESPONSE_HEADER_INITIAL_SIZE, w->statistics.memory_accounting);
    w->response.header_output = buffer_create(NETDATA_WEB_RESPONSE_HEADER_INITIAL_SIZE, w->statistics.memory_accounting);

    __atomic_add_fetch(w->statistics.memory_accounting, sizeof(struct web_client), __ATOMIC_RELAXED);

    return w;
}

void web_client_free(struct web_client *w) {
#ifdef ENABLE_HTTPS
    netdata_ssl_close(&w->ssl);
#endif

    web_client_reset_allocations(w, true);

    __atomic_sub_fetch(w->statistics.memory_accounting, sizeof(struct web_client), __ATOMIC_RELAXED);
    freez(w);
}

inline void web_client_timeout_checkpoint_init(struct web_client *w) {
    now_monotonic_high_precision_timeval(&w->timings.tv_in);
}

inline void web_client_timeout_checkpoint_set(struct web_client *w, int timeout_ms) {
    w->timings.timeout_ut = timeout_ms * USEC_PER_MS;

    if(!w->timings.tv_in.tv_sec)
        web_client_timeout_checkpoint_init(w);

    if(!w->timings.tv_timeout_last_checkpoint.tv_sec)
        w->timings.tv_timeout_last_checkpoint = w->timings.tv_in;
}

inline usec_t web_client_timeout_checkpoint(struct web_client *w) {
    struct timeval now;
    now_monotonic_high_precision_timeval(&now);

    if (!w->timings.tv_timeout_last_checkpoint.tv_sec)
        w->timings.tv_timeout_last_checkpoint = w->timings.tv_in;

    usec_t since_last_check_ut = dt_usec(&w->timings.tv_timeout_last_checkpoint, &now);

    w->timings.tv_timeout_last_checkpoint = now;

    return since_last_check_ut;
}

inline usec_t web_client_timeout_checkpoint_response_ready(struct web_client *w, usec_t *usec_since_last_checkpoint) {
    usec_t since_last_check_ut = web_client_timeout_checkpoint(w);
    if(usec_since_last_checkpoint)
        *usec_since_last_checkpoint = since_last_check_ut;

    w->timings.tv_ready = w->timings.tv_timeout_last_checkpoint;

    // return the total time of the query
    return dt_usec(&w->timings.tv_in, &w->timings.tv_ready);
}

inline bool web_client_timeout_checkpoint_and_check(struct web_client *w, usec_t *usec_since_last_checkpoint) {

    usec_t since_last_check_ut = web_client_timeout_checkpoint(w);
    if(usec_since_last_checkpoint)
        *usec_since_last_checkpoint = since_last_check_ut;

    if(!w->timings.timeout_ut)
        return false;

    usec_t since_reception_ut = dt_usec(&w->timings.tv_in, &w->timings.tv_timeout_last_checkpoint);
    if (since_reception_ut >= w->timings.timeout_ut) {
        buffer_flush(w->response.data);
        buffer_strcat(w->response.data, "Query timeout exceeded");
        w->response.code = HTTP_RESP_GATEWAY_TIMEOUT;
        return true;
    }

    return false;
}
