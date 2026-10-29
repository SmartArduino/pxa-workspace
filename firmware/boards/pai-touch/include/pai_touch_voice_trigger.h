// SPDX-License-Identifier: Apache-2.0
#pragma once
#if PXA_ENABLE_VOICE_TRIGGER
bool pai_touch_voice_trigger_initialize();
/* Only the link/heartbeat worker may make the synchronous catalog RPC. */
void pai_touch_voice_trigger_refresh();
void pai_touch_voice_trigger_link_lost();
#endif
