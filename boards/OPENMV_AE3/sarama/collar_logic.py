# collar_logic.py — registration/settings API over the fr_fr settings store.
#
# Frozen into the image alongside main.py so watcher /exec flows (which
# soft-reset the interpreter) can `import collar_logic` without the running
# app's in-memory shim. Delegates to the frozen main module — one settings
# implementation, two import names.

from main import (
    load_settings,
    create_default_settings,
    save_settings,
    get_device_identity,
)
