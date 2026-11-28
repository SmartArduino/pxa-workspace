idf_build_get_property(use_esp_openh264 PXA_USE_ESP_OPENH264)
# Espressif's P4 archives use Zb instructions. Early P4 silicon needs
# the source-built decoder even when an esp_openh264 path was supplied.
if(use_esp_openh264 AND NOT CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
    idf_build_get_property(video_board PXA_BOARD_ID)
    set(video_decode_default FAST)
    if(video_board STREQUAL "espp4-a")
        set(video_decode_default MAX_SPEED)
        set(video_board_linker LDFRAGMENTS espp4_decode.lf)
    elseif(video_board STREQUAL "esp-mosaico")
        set(video_decode_default MAX_SPEED)
        set(video_board_linker LDFRAGMENTS mosaico_decode.lf)
    elseif(video_board STREQUAL "doit-gamekit-2")
        set(video_decode_default MAX_SPEED)
        set(video_board_linker LDFRAGMENTS mosaico_decode.lf)
    endif()
    set(PXA_ESP_OPENH264_DECODE_MODE "${video_decode_default}" CACHE STRING "Espressif H.264 decode mode")
    set_property(CACHE PXA_ESP_OPENH264_DECODE_MODE PROPERTY STRINGS FAST MAX_SPEED)
    if(NOT PXA_ESP_OPENH264_DECODE_MODE MATCHES "^(FAST|MAX_SPEED)$")
        message(FATAL_ERROR "PXA_ESP_OPENH264_DECODE_MODE must be FAST or MAX_SPEED")
    endif()
    idf_component_register(SRCS src/pxa_video_codecs.cc src/video_pixels.cpp src/video_pie.S
        INCLUDE_DIRS include PRIV_INCLUDE_DIRS src "../pxa/src/services" ${video_board_linker}
        PRIV_REQUIRES pxa esp_openh264 espressif__esp_audio_codec espressif__esp_new_jpeg esp_timer esp_driver_ppa)
    if(video_board STREQUAL "esp-mosaico" AND CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP)
        target_sources(${COMPONENT_LIB} PRIVATE src/video_thread_stack.c)
        target_link_options(${COMPONENT_LIB} INTERFACE
            "-Wl,--wrap=esp_pthread_set_cfg" "-Wl,--undefined=__wrap_esp_pthread_set_cfg")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_USE_ESP_OPENH264=1
        PXA_ESP_OPENH264_DECODE_MODE=ESP_OPENH264_DEC_MODE_${PXA_ESP_OPENH264_DECODE_MODE})
    if(video_board STREQUAL "espp4-a" OR video_board STREQUAL "doit-gamekit-2")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_VIDEO_NATIVE_SURFACE=1)
    endif()
    if(video_board STREQUAL "doit-gamekit-2")
        option(PXA_GAMEKIT_BOX420_360 "Box-filter 640x360 native video at exact half scale" ON)
        if(PXA_GAMEKIT_BOX420_360)
            target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_GAMEKIT_BOX420_360=1)
        endif()
        option(PXA_GAMEKIT_VERIFY_NATIVE_OUTPUT "Check RGB565 native scanout against two-pass conversion at startup" OFF)
        if(PXA_GAMEKIT_VERIFY_NATIVE_OUTPUT)
            target_sources(${COMPONENT_LIB} PRIVATE tests/native_scanout_probe.cc)
            target_compile_definitions(${COMPONENT_LIB} PUBLIC PXA_GAMEKIT_VERIFY_NATIVE_OUTPUT=1)
        endif()
    endif()
    if((video_board STREQUAL "doit-gamekit-2" OR video_board STREQUAL "esp-mosaico")
       AND CONFIG_ESP_OPENH264_PROFILE_BALANCED
       AND PXA_ESP_OPENH264_DECODE_MODE STREQUAL "MAX_SPEED")
        if(video_board STREQUAL "doit-gamekit-2")
            option(PXA_GAMEKIT_COLD_VIDEO_IN_FLASH "Release decoder SRAM for macroblock metadata" ON)
            set(video_cold_in_flash ${PXA_GAMEKIT_COLD_VIDEO_IN_FLASH})
        else()
            option(PXA_MOSAICO_COLD_VIDEO_IN_FLASH "Release decoder SRAM for macroblock metadata" ON)
            set(video_cold_in_flash ${PXA_MOSAICO_COLD_VIDEO_IN_FLASH})
        endif()
        if(video_cold_in_flash)
            idf_component_get_property(video_vendor_dir esp_openh264 COMPONENT_DIR)
            set(video_archive "${video_vendor_dir}/libs/esp32s31/balanced/libesp_openh264.a")
            file(SHA256 "${video_archive}" video_archive_sha)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${video_archive}")
            # IDF collects dependencies' fragments before registering this component.
            # Switch this archive's placement only for the exactly profiled binary.
            idf_build_get_property(video_fragments __LDGEN_FRAGMENT_FILES)
            set(original_fragment "${video_vendor_dir}/linker.lf")
            if(video_archive_sha STREQUAL "b7d672306b1cc2b1d1ea87fa1b4ccd817055cf40ced0b888aec59e078b2bc6da"
               AND original_fragment IN_LIST video_fragments)
                list(REMOVE_ITEM video_fragments "${original_fragment}")
                list(APPEND video_fragments "${CMAKE_CURRENT_LIST_DIR}/s31_stream_balanced.lf")
                idf_build_set_property(__LDGEN_FRAGMENT_FILES "${video_fragments}")
            endif()
        endif()
    endif()
    target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
    return()
endif()

# Portable BSD-2-Clause OpenH264 decoder only; no encoder or external libraries.
set(PXA_VIDEO_H264_WORKERS "0" CACHE STRING "OpenH264 software workers (0 or 2)")
if(NOT PXA_VIDEO_H264_WORKERS MATCHES "^(0|2)$")
    message(FATAL_ERROR "PXA_VIDEO_H264_WORKERS must be 0 or 2")
endif()
set(video_vendor "${CMAKE_CURRENT_LIST_DIR}/vendor/openh264")
file(GLOB video_common "${video_vendor}/codec/common/src/*.cpp")
file(GLOB video_decoder "${video_vendor}/codec/decoder/core/src/*.cpp"
                        "${video_vendor}/codec/decoder/plus/src/*.cpp")
file(GLOB_RECURSE video_overlay_inputs "${video_vendor}/*.h" "${video_vendor}/*.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${video_overlay_inputs} "${CMAKE_CURRENT_LIST_DIR}/src/openh264_threads_overlay.inc")
# ESP-IDF has pthread condition variables but no POSIX unnamed semaphores or
# sysctl. Build an explicit platform overlay, preserving the vendored snapshot.
set(video_overlay "${CMAKE_CURRENT_BINARY_DIR}/openh264-esp")
file(MAKE_DIRECTORY "${video_overlay}")
file(COPY "${video_vendor}/codec/common/inc/" DESTINATION "${video_overlay}")
file(READ "${video_vendor}/codec/common/inc/WelsThreadLib.h" video_thread_header)
string(REPLACE "#ifdef __APPLE__" "#if defined(__APPLE__) || defined(ESP_PLATFORM)" video_thread_header "${video_thread_header}")
file(WRITE "${video_overlay}/WelsThreadLib.h" "${video_thread_header}")
file(READ "${video_vendor}/codec/common/src/WelsThreadLib.cpp" video_threads)
string(REPLACE "#elif !defined(__Fuchsia__)" "#elif !defined(__Fuchsia__) && !defined(ESP_PLATFORM)" video_threads "${video_threads}")
string(REPLACE "#if !defined(__ANDROID__) && !defined(__Fuchsia__)" "#if !defined(__ANDROID__) && !defined(__Fuchsia__) && !defined(ESP_PLATFORM)" video_threads "${video_threads}")
string(REPLACE "#ifdef __APPLE__" "#if defined(__APPLE__) || defined(ESP_PLATFORM)" video_threads "${video_threads}")
string(REPLACE "#if defined(__APPLE__)" "#if defined(__APPLE__) || defined(ESP_PLATFORM)" video_threads "${video_threads}")
string(REPLACE "#ifdef ANDROID_NDK\n  pInfo" "#if defined(ESP_PLATFORM)\n  pInfo->ProcessorCount = 2;\n  return WELS_THREAD_ERROR_OK;\n#elif defined(ANDROID_NDK)\n  pInfo" video_threads "${video_threads}")
string(PREPEND video_threads "#include \"pxa_video_threads.h\"\n")
string(REPLACE "pthread_create (thread, &at, routine, arg)" "pxa_video_pthread_create(thread, &at, routine, arg)" video_threads "${video_threads}")
string(REPLACE "pthread_join (thread, NULL)" "pxa_video_pthread_join(thread, NULL)" video_threads "${video_threads}")
file(WRITE "${video_overlay}/WelsThreadLib.cpp" "${video_threads}")
list(FILTER video_common EXCLUDE REGEX "/WelsThreadLib.cpp$")
file(READ "${video_vendor}/codec/decoder/core/src/wels_decoder_thread.cpp" video_dec_threads)
string(PREPEND video_dec_threads "#include \"pxa_video_row_events.hpp\"\n")
string(REPLACE "int EventCreate (SWelsDecEvent* e, int manualReset, int initialState) {"
    "int EventCreate (SWelsDecEvent* e, int manualReset, int initialState) {\n#ifdef ESP_PLATFORM\n  if(manualReset==3) {e->manualReset=3;__atomic_store_n(&e->isSignaled,initialState,__ATOMIC_RELAXED);return 0;}\n#endif"
    video_dec_threads "${video_dec_threads}")
string(REPLACE "void EventReset (SWelsDecEvent* e) {"
    "void EventReset (SWelsDecEvent* e) {\n#ifdef ESP_PLATFORM\n  if(e->manualReset==3) {__atomic_store_n(&e->isSignaled,0,__ATOMIC_RELAXED);return;}\n#endif"
    video_dec_threads "${video_dec_threads}")
string(REPLACE "void EventPost (SWelsDecEvent* e) {"
    "void EventPost (SWelsDecEvent* e) {\n#ifdef ESP_PLATFORM\n  if(e->manualReset==3) {__atomic_store_n(&e->isSignaled,1,__ATOMIC_RELEASE);return;}\n#endif"
    video_dec_threads "${video_dec_threads}")
string(REPLACE "int EventWait (SWelsDecEvent* e, int32_t timeout) {"
    "int EventWait (SWelsDecEvent* e, int32_t timeout) {\n#ifdef ESP_PLATFORM\n  if(e->manualReset==3)return pxa_video_wait_row(e,timeout);\n#endif"
    video_dec_threads "${video_dec_threads}")
string(REPLACE "void EventDestroy (SWelsDecEvent* e) {"
    "void EventDestroy (SWelsDecEvent* e) {\n#ifdef ESP_PLATFORM\n  if(e->manualReset==3)return;\n#endif"
    video_dec_threads "${video_dec_threads}")
string(REPLACE "#ifdef __APPLE__" "#if defined(__APPLE__) || defined(ESP_PLATFORM)" video_dec_threads "${video_dec_threads}")
string(REPLACE "#if defined(__APPLE__)" "#if defined(__APPLE__) || defined(ESP_PLATFORM)" video_dec_threads "${video_dec_threads}")
file(WRITE "${video_overlay}/wels_decoder_thread.cpp" "${video_dec_threads}")
list(FILTER video_decoder EXCLUDE REGEX "/wels_decoder_thread.cpp$")
# Newlib uses long for int32_t. Match the upstream header's exact type.
file(READ "${video_vendor}/codec/decoder/core/src/decoder_core.cpp" video_core)
string(REPLACE "const int kiSrcLen)" "const int32_t kiSrcLen)" video_core "${video_core}")
string(REPLACE "const int kiCurrLen)" "const int32_t kiCurrLen)" video_core "${video_core}")
file(WRITE "${video_overlay}/decoder_core.cpp" "${video_core}")
list(FILTER video_decoder EXCLUDE REGEX "/decoder_core.cpp$")
# Respect a valid AVC decoded-picture-buffer bound instead of always allocating
# all 16 reference pictures for two workers on a memory-constrained device.
file(READ "${video_vendor}/codec/decoder/core/src/decoder.cpp" video_decoder_control)
string(PREPEND video_decoder_control "#include \"pxa_video_dpb.hpp\"\n")
string(REPLACE "iNumRefFrames = MAX_DPB_COUNT + iThreadCount;"
    "iNumRefFrames = pxa_video::threaded_picture_count(pCtx->pSps->iNumRefFrames, pCtx->pSps->sVui.uiMaxNumReorderFrames, pCtx->pSps->sVui.uiMaxDecFrameBuffering, pCtx->pSps->bVuiParamPresentFlag && pCtx->pSps->sVui.bBitstreamRestrictionFlag, iThreadCount, MAX_DPB_COUNT);"
    video_decoder_control "${video_decoder_control}")
file(WRITE "${video_overlay}/decoder.cpp" "${video_decoder_control}")
list(FILTER video_decoder EXCLUDE REGEX "/decoder.cpp$")
file(READ "${video_vendor}/codec/decoder/core/src/pic_queue.cpp" video_pictures)
string(REPLACE "  if (pCtx->pThreadCtx != NULL) {" [=[
  if (!pPic->pMbCorrectlyDecodedFlag || (GetThreadCount(pCtx)>1 && !pPic->pNzc) ||
      !pPic->pMbType || !pPic->pMv[0] || !pPic->pMv[1] || !pPic->pRefIndex[0] || !pPic->pRefIndex[1]) {
    FreePicture(pPic,pMa); return NULL;
  }
  if (pCtx->pThreadCtx != NULL) {]=] video_pictures "${video_pictures}")
string(REPLACE "    for (uint32_t i = 0; i < uiMbHeight; ++i) {\n      CREATE_EVENT (&pPic->pReadyEvent[i], 1, 0, NULL);\n    }" [=[
    if (!pPic->pReadyEvent) { FreePicture(pPic,pMa); return NULL; }
    for (uint32_t i = 0; i < uiMbHeight; ++i) {
      if(CREATE_EVENT (&pPic->pReadyEvent[i], 3, 0, NULL)) {
        for(uint32_t j=0;j<i;++j)CLOSE_EVENT(&pPic->pReadyEvent[j]);
        pMa->WelsFree(pPic->pReadyEvent,"pPic->pReadyEvent");pPic->pReadyEvent=NULL;
        FreePicture(pPic,pMa);return NULL;
      }
    }]=] video_pictures "${video_pictures}")
string(REPLACE "  return pPic;\n}\n\nvoid FreePicture" "  if(pCtx->pThreadCtx && !pPic->pRowMbDone) {FreePicture(pPic,pMa);return NULL;}\n  return pPic;\n}\n\nvoid FreePicture" video_pictures "${video_pictures}")
file(WRITE "${video_overlay}/pic_queue.cpp" "${video_pictures}")
list(FILTER video_decoder EXCLUDE REGEX "/pic_queue.cpp$")
# Decoder control objects must also fail without throwing/aborting when
# the app's memory quota is exhausted. Keep the vendor snapshot intact.
file(READ "${video_vendor}/codec/decoder/plus/src/welsDecoderExt.cpp" video_ext)
string(PREPEND video_ext "#include \"pxa_video_memory.h\"\n#include \"pxa_video_threads.h\"\n")
string(REPLACE "if (ResetDecoder (pDecContext)) {" "if (GetThreadCount(pDecContext)>1 || ResetDecoder (pDecContext)) {" video_ext "${video_ext}")
string(REPLACE "      ConstructAccessUnit (pWelsDecoder, pThrCtx);" [=[
      if(ConstructAccessUnit (pWelsDecoder, pThrCtx)!=dsErrorFree) {
        pxa_video_worker_error();
        SET_EVENT(&pThrCtx->sSliceDecodeStart);SET_EVENT(&pThrCtx->sSliceDecodeFinish);
        if(pThrCtx->pCtx && pThrCtx->pCtx->pDec && pThrCtx->pCtx->pDec->pReadyEvent)
          for(uint32_t row=0;row<(pThrCtx->pCtx->pDec->iHeightInPixel+15u)/16u;++row)
            SET_EVENT(&pThrCtx->pCtx->pDec->pReadyEvent[row]);
      }]=] video_ext "${video_ext}")
string(REPLACE "new welsCodecTrace()" "pxa_video_new<welsCodecTrace>()" video_ext "${video_ext}")
string(REPLACE "new SWelsDecoderThreadCTX[m_iCtxCount]" "pxa_video_array<SWelsDecoderThreadCTX>(m_iCtxCount)" video_ext "${video_ext}")
string(REPLACE "new CMemoryAlign (iCacheLineSize)" "pxa_video_new<CMemoryAlign>(iCacheLineSize)" video_ext "${video_ext}")
string(REPLACE "WELS_VERIFY_RETURN_PROC_IF (cmMallocMemeError, (NULL == pCtx->pMemAlign), UninitDecoderCtx (pCtx))" [=[if (!pCtx->pMemAlign) { WelsFree(pCtx, "m_pDecContext"); pCtx = NULL; return cmMallocMemeError; }]=] video_ext "${video_ext}")
string(REPLACE "new CWelsDecoder()" "pxa_video_new<CWelsDecoder>()" video_ext "${video_ext}")
string(REPLACE "memset (m_pDecThrCtx, 0," "if (m_pDecThrCtx) memset (m_pDecThrCtx, 0," video_ext "${video_ext}")
string(REPLACE "if (m_pWelsTrace == NULL)" "if (m_pWelsTrace == NULL || m_pDecThrCtx == NULL)" video_ext "${video_ext}")
string(REPLACE "void CWelsDecoder::UninitDecoder (void) {" "void CWelsDecoder::UninitDecoder (void) {\n  if (!m_pDecThrCtx) return;" video_ext "${video_ext}")
string(REPLACE "delete m_pWelsTrace" "pxa_video_delete(m_pWelsTrace)" video_ext "${video_ext}")
string(REPLACE "delete[] m_pDecThrCtx" "pxa_video_free(m_pDecThrCtx)" video_ext "${video_ext}")
string(REPLACE "delete [] m_pDecThrCtx" "pxa_video_free(m_pDecThrCtx)" video_ext "${video_ext}")
string(REPLACE "delete pCtx->pMemAlign" "pxa_video_delete(pCtx->pMemAlign)" video_ext "${video_ext}")
string(REPLACE "  WelsEndDecoder (pCtx);" "  if (pCtx->pParam) WelsEndDecoder (pCtx);" video_ext "${video_ext}")
string(REPLACE "delete (CWelsDecoder*)pDecoder" "pxa_video_delete((CWelsDecoder*)pDecoder)" video_ext "${video_ext}")
string(REPLACE "    InitDecoderCtx (m_pDecThrCtx[i].pCtx, pParam);" "    int32_t pxa_context_status = InitDecoderCtx (m_pDecThrCtx[i].pCtx, pParam);\n    if (pxa_context_status != cmResultSuccess) { UninitDecoder(); return pxa_context_status; }" video_ext "${video_ext}")
file(READ "${video_vendor}/codec/decoder/plus/inc/welsDecoderExt.h" video_ext_header)
string(REPLACE "void OpenDecoderThreads();" "int32_t OpenDecoderThreads();\n  unsigned m_pxa_threads_created=0;\n  unsigned char m_pxa_global_sync=0,m_pxa_thread_sync[WELS_DEC_MAX_NUM_CPU]{};" video_ext_header "${video_ext_header}")
file(WRITE "${video_overlay}/welsDecoderExt.h" "${video_ext_header}")
file(READ "${CMAKE_CURRENT_LIST_DIR}/src/openh264_threads_overlay.inc" video_thread_control)
string(FIND "${video_ext}" "void CWelsDecoder::OpenDecoderThreads()" video_thread_begin)
string(FIND "${video_ext}" "void CWelsDecoder::UninitDecoderCtx" video_thread_end)
string(SUBSTRING "${video_ext}" 0 ${video_thread_begin} video_thread_prefix)
string(SUBSTRING "${video_ext}" ${video_thread_end} -1 video_thread_suffix)
set(video_ext "${video_thread_prefix}${video_thread_control}\n${video_thread_suffix}")
string(REPLACE "  OpenDecoderThreads();" "  if(OpenDecoderThreads()!=cmResultSuccess)return cmMallocMemeError;" video_ext "${video_ext}")
file(WRITE "${video_overlay}/welsDecoderExt.cpp" "${video_ext}")
list(FILTER video_decoder EXCLUDE REGEX "/welsDecoderExt.cpp$")
# All bulk malloc/free in the codec use the owning application's resource budget.
file(READ "${video_vendor}/codec/common/src/memory_align.cpp" video_memory)
string(REPLACE "#include <stdlib.h>" "#include <stdlib.h>\n#include \"pxa_video_memory.h\"" video_memory "${video_memory}")
string(REGEX REPLACE "([^A-Za-z_])malloc \\(" "\\1pxa_video_malloc (" video_memory "${video_memory}")
string(REGEX REPLACE "([^A-Za-z_])free \\(" "\\1pxa_video_free (" video_memory "${video_memory}")
file(WRITE "${video_overlay}/memory_align.cpp" "${video_memory}")
list(FILTER video_common EXCLUDE REGEX "/memory_align.cpp$")
file(READ "${video_vendor}/codec/common/src/mc.cpp" video_mc)
string(PREPEND video_mc [=[
#include <stdint.h>
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3
extern "C" void pxa_video_pie_average16(unsigned char*, int32_t, const unsigned char*, int32_t,
    const unsigned char*, int32_t, int32_t, int32_t);
extern "C" void pxa_video_pie_halfpel(const unsigned char*, int32_t, unsigned char*, int32_t,
    int32_t, int32_t, int32_t);
extern "C" void pxa_video_pie_chroma8(const unsigned char*, int32_t, unsigned char*, int32_t,
    const unsigned char*, int32_t);
#endif
]=])
string(REPLACE "  int32_t i, j;\n  for (i = 0; i < iHeight; i++) {\n    for (j = 0; j < iWidth; j++) {\n      pDst[j] = (pSrcA[j] + pSrcB[j] + 1) >> 1;"
    [=[
#if CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3
  if (iWidth==16 && iHeight>0) {
    pxa_video_pie_average16(pDst,iDstStride,pSrcA,iSrcAStride,pSrcB,iSrcBStride,iWidth,iHeight);return;
  }
#endif
  int32_t i, j;
  for (i = 0; i < iHeight; i++) {
    for (j = 0; j < iWidth; j++) {
      pDst[j] = (pSrcA[j] + pSrcB[j] + 1) >> 1;]=] video_mc "${video_mc}")
foreach(video_tap_step "1" "iSrcStride")
    set(video_filter_loop "  int32_t i, j;\n  for (i = 0; i < iHeight; i++) {\n    for (j = 0; j < iWidth; j++) {\n      pDst[j] = WelsClip1 ((FilterInput8bitWithStride_c (pSrc + j, ${video_tap_step}) + 16) >> 5);")
    string(REPLACE "${video_filter_loop}"
        "#if CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3\n  if ((iWidth==8 || iWidth==16) && iHeight>0) {\n    pxa_video_pie_halfpel(pSrc,iSrcStride,pDst,iDstStride,iWidth,iHeight,${video_tap_step});return;\n  }\n#endif\n${video_filter_loop}"
        video_mc "${video_mc}")
endforeach()
string(REPLACE "  iA = pABCD[0];"
    "#if CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3\n  if (iWidth==8 && iHeight>0) {\n    pxa_video_pie_chroma8(pSrc,iSrcStride,pDst,iDstStride,pABCD,iHeight);return;\n  }\n#endif\n  iA = pABCD[0];"
    video_mc "${video_mc}")
file(WRITE "${video_overlay}/mc.cpp" "${video_mc}")
list(FILTER video_common EXCLUDE REGEX "/mc.cpp$")
idf_component_register(SRCS src/pxa_video_codecs.cc src/video_pixels.cpp src/video_pie.S src/pxa_video_threads.cc ${video_common} ${video_decoder}
    "${video_overlay}/WelsThreadLib.cpp" "${video_overlay}/memory_align.cpp"
    "${video_overlay}/wels_decoder_thread.cpp" "${video_overlay}/decoder_core.cpp"
    "${video_overlay}/decoder.cpp"
    "${video_overlay}/pic_queue.cpp"
    "${video_overlay}/welsDecoderExt.cpp"
    "${video_overlay}/mc.cpp"
    INCLUDE_DIRS include
    LDFRAGMENTS video_hot.lf
    PRIV_INCLUDE_DIRS src "${video_overlay}" "${video_vendor}/codec/api" "${video_vendor}/codec/api/wels"
        "${video_vendor}/codec/common/inc" "${video_vendor}/codec/decoder/core/inc"
        "${video_vendor}/codec/decoder/plus/inc"
        "../pxa/src/services"
    PRIV_REQUIRES pxa espressif__esp_audio_codec espressif__esp_new_jpeg esp_timer pthread esp_driver_ppa)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -fno-strict-aliasing -Wno-error -Wno-unused-function)
target_compile_definitions(${COMPONENT_LIB} PRIVATE WELS_DECODER_ONLY=1 PXA_VIDEO_H264_WORKERS=${PXA_VIDEO_H264_WORKERS})
