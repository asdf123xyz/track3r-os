# Module SDK guide

The guide to writing a Community OS module (instrument, effect or app) is **[module/sdk/README.md](../../module/sdk/README.md)**, next to the ABI header it describes (MIT licensed, like the
rest of the SDK). It covers the flow (spec, generate, implement, host oracle tests, pack, install, `diag mod`), the manifest (`module.json`) and generator (`tools/o3m_gen.py`), the
templates (`modules/templates/`), registering and testing a module in the repository (`o3m_add_module`, `o3m_manifest`, `o3m_add_test`, the test helpers), how big a module's hot part may be, the draw API and its
budgets (with what each call costs), apps, versioning, the trust model, and a list of what is still design-only. Worked examples: the TB-303, and the Chip and FM instruments (`modules/chip/`, `modules/fm/`, OS-012c), written from
scratch by following the guide.

Design background and decisions: [ADR 0002](../adr/0002-sd-plugins.md) (amendment 1: module screens; amendment 2: as built, OS-012b; amendment 3: the module version in the file header, ABI 1.2, OS-MODVER).

the SD-module design guide (the design-time guide, if your checkout has it) predates the loader, the packer, ABI 1.1 and the module version (ABI 1.2: a card copy replaces a built-in module only with a higher version, section 1 step 6 and section 8 of the SDK guide): where it says a piece is "not built" (header layout, packer,
loader, MODULES screen, parameters on the module's pages), the SDK guide above is current.

ABI 1.8 (OS-MODPITCH, OS-INSTUI S10): `pitch()` for the Slide and Glide FX, `O3M_NOTE_RAMP`, page names, INT value labels and the descriptor sized per minor (`O3M_MODULE_SIZE`): README section 8, ADR 0002 amendment 5.
