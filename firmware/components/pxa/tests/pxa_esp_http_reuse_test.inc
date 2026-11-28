/* Exercises the public backend entries and the actual exchange scheduler.
 * Each mock connection has an independent IDF callback target and read cursor. */
static pxa_bytes_t range_text(const char *s) {
    return (pxa_bytes_t){(const uint8_t *)s, strlen(s)};
}

static void check_range_body(pxa_net_backend_t *backend, void *exchange) {
    uint8_t bytes[512];
    for (unsigned offset = 0; offset < 8192; offset += sizeof(bytes)) {
        assert(backend->exchange_io(NULL, exchange, PXA_IO_READ, bytes, sizeof(bytes)) == sizeof(bytes));
        for (unsigned i = 0; i < sizeof(bytes); ++i) assert(bytes[i] == (uint8_t)(offset+i));
    }
}

static void range_request(pxa_net_backend_t *backend, pxa_net_request_t *request,
                          unsigned fresh, unsigned reused, pxa_status_t expected) {
    void *exchange;
    unsigned inits = mock_inits, takes = reused_opens;
    assert(backend->open_exchange(NULL, request, &exchange) == PXA_STATUS_OK);
    for (unsigned i = 0; i < 16 && queued_count; ++i) step_stream();
    assert(!queued_count && mock_inits - inits == fresh && reused_opens - takes == reused);
    assert(http_stream(exchange)->stream.error == expected);
    if (!expected) check_range_body(backend, exchange);
    backend->close_exchange(NULL, exchange);
    step_stream();
    assert(!queued_count && live_allocations == 1 && g_net->http_clients == 0);
}

static void test_http_connection_reuse(pxa_net_backend_t *backend) {
    pxa_net_header_t headers[] = {{range_text("Range"),range_text("bytes=0-8191")},
                                  {range_text("Accept"),range_text("*/*")}};
    pxa_net_request_t request = {.struct_size=sizeof(request), .method=PXA_NET_METHOD_GET,
        .abi_minor=2, .timeout_ms=2000, .max_body_bytes=262144, .buffer_bytes=32768,
        .response_header_bytes=512, .url=range_text("https://media.example.test/a?q=1"),
        .headers=headers, .header_count=2, .component=3};
    mock_mode=4; expected_rx_buffer=4096;
    void *reserved = NULL;
    reset_on_alloc = true;
    assert(backend->open_exchange(NULL,&request,&reserved)==PXA_STATUS_CANCELLED);
    assert(!reserved && live_allocations==1 && !queued_count);
    range_request(backend,&request,1,0,PXA_STATUS_OK);
    assert(g_net->idle.client && http_idle_wait(g_net)!=portMAX_DELAY);
    request.url=range_text("https://media.example.test/b?q=2");
    headers[0].value=range_text("bytes=8192-16383");
    range_request(backend,&request,0,1,PXA_STATUS_OK);
    for (unsigned i=0;i<2;++i) {
        request.component++;
        range_request(backend,&request,1,0,PXA_STATUS_OK);
    }
    request.header_count=1;
    range_request(backend,&request,1,0,PXA_STATUS_OK);
    request.url=range_text("https://media.example.test.evil.test/c");
    range_request(backend,&request,1,0,PXA_STATUS_OK);
    request.url=range_text("https://media.example.test:444/c");
    range_request(backend,&request,1,0,PXA_STATUS_OK);
    request.struct_size=PXA_NET_REQUEST_BASE_BYTES;
    range_request(backend,&request,1,0,PXA_STATUS_OK); assert(!g_net->idle.client);
    request.struct_size=sizeof(request); request.header_count=2;
    const char *private_names[]={"Cookie","Authorization","Proxy-Authorization","Host","Connection",
        "If-Range","X-Anything","Range"};
    for (unsigned i=0;i<sizeof(private_names)/sizeof(*private_names);++i) {
        headers[1].name=range_text(private_names[i]);
        if (pxa_net_header_name_valid(headers[1].name, 1)) {
            range_request(backend,&request,1,0,PXA_STATUS_OK);
        } else {
            void *exchange;
            assert(backend->open_exchange(NULL,&request,&exchange)==PXA_STATUS_INVALID_ARGUMENT);
        }
        assert(!g_net->idle.client);
    }
    headers[1].name=range_text("Accept");
    const char *private_responses[]={"Set-Cookie","Set-Cookie2","WWW-Authenticate",
        "Proxy-Authenticate","Cache-Control","Location"};
    for (unsigned i=0;i<sizeof(private_responses)/sizeof(*private_responses);++i) {
        response_header=private_responses[i];
        range_request(backend,&request,1,0,PXA_STATUS_OK); assert(!g_net->idle.client);
    }
    response_header=NULL;
    range_status=302;
    range_request(backend,&request,1,0,PXA_STATUS_OK); assert(!g_net->idle.client);
    range_status=206; persistent=false;
    range_request(backend,&request,1,0,PXA_STATUS_OK); assert(!g_net->idle.client);
    persistent=true;
    range_request(backend,&request,1,0,PXA_STATUS_OK);
    reject_update=1;
    range_request(backend,&request,1,0,PXA_STATUS_OK);
    stale_open=1;
    range_request(backend,&request,1,1,PXA_STATUS_OK);
    stale_head=1;
    range_request(backend,&request,1,1,PXA_STATUS_OK);
    fail_after_header=1;
    range_request(backend,&request,0,1,PXA_STATUS_PROTOCOL_ERROR);
    assert(!g_net->idle.client);
    range_request(backend,&request,1,0,PXA_STATUS_OK);
    stale_open=2;
    range_request(backend,&request,1,1,PXA_STATUS_IO_ERROR);
    assert(!g_net->idle.client && !stale_open);
    range_request(backend,&request,1,0,PXA_STATUS_OK);
    mock_time+=PXA_HTTP_IDLE_US;
    assert(http_idle_wait(g_net)==portMAX_DELAY && !g_net->idle.client);
    range_request(backend,&request,1,0,PXA_STATUS_OK);
    low_heap=true;
    assert(http_idle_wait(g_net)==portMAX_DELAY && !g_net->idle.client);
    range_request(backend,&request,1,0,PXA_STATUS_OK); assert(!g_net->idle.client);
    low_heap=false;
    range_request(backend,&request,1,0,PXA_STATUS_OK);
    pxa_esp_net_reset_requests();
    range_request(backend,&request,1,0,PXA_STATUS_OK);
    void *exchanges[4];
    for (unsigned round=0;round<64;++round) {
        for (unsigned i=0;i<4;++i) assert(!backend->open_exchange(NULL,&request,&exchanges[i]));
        for (unsigned i=0;i<4;++i) step_stream();
        assert(g_net->http_clients==4 && !g_net->idle.client);
        if (round%2==0) {
            pxa_esp_net_reset_requests();
            for (unsigned i=0;i<4;++i) step_stream();
            assert(!g_net->idle.client);
        } else {
            for (unsigned i=0;i<32 && queued_count;++i) step_stream();
            assert(!queued_count && g_net->idle.client);
            for (unsigned i=0;i<4;++i) {
                check_range_body(backend,exchanges[i]);
                backend->close_exchange(NULL,exchanges[i]);
            }
            for (unsigned i=0;i<4;++i) step_stream();
        }
        assert(!queued_count && live_allocations==1 && !g_net->http_clients);
    }
    pxa_esp_net_reset_requests();
    assert(http_idle_wait(g_net)==portMAX_DELAY && mock_cleanups==mock_inits);
    mock_mode=0; expected_rx_buffer=512;
}
