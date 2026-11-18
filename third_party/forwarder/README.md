# HOME Menu forwarder

The loader is nx-hbloader (ISC, `LICENSE.md`) with two changes: it starts `sdmc:/switch/SwitchMX/SwitchMX.nro`, and
it returns to the HOME Menu when the app exits. `SwitchMX.json` sets the title ID.

`source/forwarder_data.c` holds its files inside plain templates of the three content archives (NCAs). No keys are
in the templates: the app finishes them on the console, using that console's own `prod.keys`. See
`tools/gen_forwarder_data.sh`.
