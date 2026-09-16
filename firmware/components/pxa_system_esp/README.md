# pxa_system_esp

ESP product adapter for PXA System. It mirrors the verified PXA package store
into the portable application registry, routes shell launches through the
common task manager, and marshals PXA worker lifecycle notifications onto the
LVGL owner thread. PXA Guest system operations use service `17`: Intent, RPC,
topic publication/subscription, and exported PXA endpoints all enter the same
portable registries used by native applications. Requests are copied to the
system owner thread and completions or reliable events are queued back to the
PXA worker, with caller identity derived from the verified manifest.

The bridge also observes the portable theme service and maps its effective
light/dark scheme into the standard PXA UI environment for both active and
subsequently launched Guests.

The bridge keeps PXA instances alive across asynchronous stop, and owns all
Guest subscriptions, exported endpoints, and pending inbound calls so they can
be cancelled and removed before instance destruction. Capacities are explicit
in `pxsys_esp_pxa_bridge_config_t`.

This component is platform glue. Portable core, application, and protocol code
must not depend on it.
