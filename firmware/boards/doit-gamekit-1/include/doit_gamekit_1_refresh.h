#pragma once

namespace doit_gamekit_1 {
// UI-task-owned classification of LVGL refreshes. INVALIDATE_AREA is also
// emitted for buffer rounding during rendering; those events are not content.
class RefreshTracker {
public:
    void Invalidated() {
        if (!rendering_ && !issuing_diagnostic_) content_pending_ = true;
    }
    void BeginDiagnosticInvalidation() {
        diagnostic_pending_ = true;
        issuing_diagnostic_ = true;
    }
    void EndDiagnosticInvalidation() { issuing_diagnostic_ = false; }
    void BeginRender() {
        content_ = content_pending_ || !diagnostic_pending_;
        content_pending_ = diagnostic_pending_ = false;
        rendering_ = true;
    }
    void EndRender() { rendering_ = false; }
    bool content() const { return content_; }
private:
    bool content_pending_ = true, diagnostic_pending_ = false;
    bool issuing_diagnostic_ = false, rendering_ = false, content_ = true;
};
}
