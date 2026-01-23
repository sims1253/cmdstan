# FlatCC Library

This directory should contain the FlatCC library (FlatBuffers C compiler), required for Arrow IPC format support via nanoarrow.

## Installation

\`\`\`bash
git clone https://github.com/dvidelern/flatcc.git flatcc
\`\`\`

Or download from: https://github.com/dvidelern/flatcc

## Building

The runtime library needs to be compiled:

\`\`\`bash
cd flatcc/src/runtime
# Compile the required .o files:
cc -c -I../../include builder.c emitter.c refmap.c verifier.c json_parser.c json_printer.c
mv *.o ../../
\`\`\`

Required object files: \`builder.o\`, \`emitter.o\`, \`refmap.o\`, \`verifier.o\`, \`json_parser.o\`, \`json_printer.o\`
