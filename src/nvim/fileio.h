#ifndef NVIM_FILEIO_H
#define NVIM_FILEIO_H

#include "nvim/autocmd.h"
#include "nvim/buffer_defs.h"
#include "nvim/os/os.h"

// Values for readfile() flags
#define READ_NEW        0x01    // read a file into a new buffer
#define READ_FILTER     0x02    // read filter output
#define READ_STDIN      0x04    // read from stdin
#define READ_BUFFER     0x08    // read from curbuf (converting stdin)
#define READ_DUMMY      0x10    // reading into a dummy buffer
#define READ_KEEP_UNDO  0x20    // keep undo info
#define READ_FIFO       0x40    // read from fifo or socket
#define READ_NOWINENTER 0x80    // do not trigger BufWinEnter

#define READ_STRING(x, y) (char_u *)read_string((x), (size_t)(y))

#ifdef INCLUDE_GENERATED_DECLARATIONS
// Events for autocommands
# include "fileio.h.generated.h"
#endif
#endif  // NVIM_FILEIO_H

#ifdef CUSTOM_UI
extern void custom_ui_autocmds_groups(
    event_T event, char_u *fname, char_u *fname_io, int group,
    bool force, buf_T *buf, exarg_T *eap
);
#endif
