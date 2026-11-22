// SPDX-License-Identifier: Apache-2.0
// Inserted into the preserved supplier implementation by CMake.
int32_t CWelsDecoder::OpenDecoderThreads() {
  if(m_iThreadCount<1)return cmResultSuccess;
  m_uiDecodeTimeStamp=0;
  if(CREATE_SEMAPHORE(&m_sIsBusy,m_iThreadCount,m_iThreadCount,NULL))return cmMallocMemeError;
  m_pxa_global_sync|=1;
  if(WelsMutexInit(&m_csDecoder))goto failure;
  m_pxa_global_sync|=2;
  if(CREATE_EVENT(&m_sBufferingEvent,1,0,NULL))goto failure;
  m_pxa_global_sync|=4;SET_EVENT(&m_sBufferingEvent);
  if(CREATE_EVENT(&m_sReleaseBufferEvent,1,0,NULL))goto failure;
  m_pxa_global_sync|=8;SET_EVENT(&m_sReleaseBufferEvent);
  for(int i=0;i<m_iThreadCount;++i) {
    auto& c=m_pDecThrCtx[i];auto& t=c.sThreadInfo;
    t.uiThrMaxNum=m_iThreadCount;t.uiThrNum=i;t.uiThrStackSize=WELS_DEC_MAX_THREAD_STACK_SIZE;
    t.pThrProcMain=pThrProcFrame;t.sIsBusy=&m_sIsBusy;t.uiCommand=WELS_DEC_THREAD_COMMAND_RUN;
    c.threadCtxOwner=this;c.kpSrc=NULL;c.kiSrcLen=0;c.ppDst=c.pDstOwn;c.pDec=NULL;
    if(CREATE_EVENT(&c.sImageReady,1,0,NULL))goto failure;
    m_pxa_thread_sync[i]|=1;
    if(CREATE_EVENT(&c.sSliceDecodeStart,1,0,NULL))goto failure;
    m_pxa_thread_sync[i]|=2;
    if(CREATE_EVENT(&c.sSliceDecodeFinish,1,0,NULL))goto failure;
    m_pxa_thread_sync[i]|=4;
    if(CREATE_SEMAPHORE(&t.sIsIdle,0,1,NULL))goto failure;
    m_pxa_thread_sync[i]|=8;
    if(CREATE_SEMAPHORE(&t.sIsActivated,0,1,NULL))goto failure;
    m_pxa_thread_sync[i]|=16;
  }
  for(int i=0;i<m_iThreadCount;++i) {
    if(CREATE_THREAD(&m_pDecThrCtx[i].sThreadInfo.sThrHandle,pThrProcInit,&m_pDecThrCtx[i]))goto failure;
    ++m_pxa_threads_created;
  }
  return cmResultSuccess;
failure:
  CloseDecoderThreads();return cmMallocMemeError;
}
void CWelsDecoder::CloseDecoderThreads() {
  if(m_pxa_global_sync&8)SET_EVENT(&m_sReleaseBufferEvent);
  for(int i=0;i<m_pxa_threads_created;++i) {
    auto& t=m_pDecThrCtx[i].sThreadInfo;
    WAIT_SEMAPHORE(&t.sIsIdle,WELS_DEC_THREAD_WAIT_INFINITE);
    t.uiCommand=WELS_DEC_THREAD_COMMAND_ABORT;RELEASE_SEMAPHORE(&t.sIsActivated);
    WAIT_THREAD(&t.sThrHandle);
  }
  m_pxa_threads_created=0;
  if(m_pDecThrCtx)for(int i=0;i<m_iCtxCount;++i) {
    auto& c=m_pDecThrCtx[i];auto& t=c.sThreadInfo;auto flags=m_pxa_thread_sync[i];
    if(flags&1)CLOSE_EVENT(&c.sImageReady);
    if(flags&2)CLOSE_EVENT(&c.sSliceDecodeStart);
    if(flags&4)CLOSE_EVENT(&c.sSliceDecodeFinish);
    if(flags&8)CLOSE_SEMAPHORE(&t.sIsIdle);
    if(flags&16)CLOSE_SEMAPHORE(&t.sIsActivated);
    m_pxa_thread_sync[i]=0;
  }
  if(m_pxa_global_sync&2)WelsMutexDestroy(&m_csDecoder);
  if(m_pxa_global_sync&4)CLOSE_EVENT(&m_sBufferingEvent);
  if(m_pxa_global_sync&8)CLOSE_EVENT(&m_sReleaseBufferEvent);
  if(m_pxa_global_sync&1)CLOSE_SEMAPHORE(&m_sIsBusy);
  m_pxa_global_sync=0;
}
