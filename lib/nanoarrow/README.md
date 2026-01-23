# nanoarrow Library

This directory should contain the nanoarrow library, required for Arrow IPC format support.

## Installation

Download the bundled release from: https://github.com/apache/arrow-nanoarrow/releases

You need:
- \`nanoarrow.h\`
- \`nanoarrow.c\`  
- \`nanoarrow_ipc.h\` (from the IPC extension)
- \`nanoarrow_ipc.c\`

## Building

\`\`\`bash
cc -c -I. nanoarrow.c nanoarrow_ipc.c
\`\`\`

Required object files: \`nanoarrow.o\`, \`nanoarrow_ipc.o\`
