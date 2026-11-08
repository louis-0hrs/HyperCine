# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Complete public worker seam; the existing VM worker stays host-owned."""
from . import controller

def poll(api,worker_config,state,reporter_factory=controller.ProductionReporter):
    cfg=worker_config.get('premerge',{})
    if cfg.get('enabled') is not True:return
    return controller.cycle(api,cfg,state,reporter_factory=reporter_factory)
