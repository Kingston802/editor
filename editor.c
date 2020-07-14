/*** includes ***/

/* above includes because macros will determine what features to expose */ 
#define _DEFAULT_SOURCE 
#define _BSD_SOURCE
#define _GNU_SOURCE 


#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/
#define EDITOR_VERSION "0.0.1"
#define EDITOR_TAB_STOP 2
#define EDITOR_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

/* defining the first as 1000, the following become 1001, 1002 etc */ 
/* we use large int values (above the limit of char) to represent arrow keys so they don't conflict */ 
enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/* every character that is part */ 
/* of a number will get a */ 
/* corresponding HL_NUMBER */
/* every other value in hl */
/* will be HL_NORMAL */
enum editorHighlight {
  HL_NORMAL = 0,
  HL_COMMENT,
  HL_MLCOMMENT,
  HL_KEYWORD1,
  HL_KEYWORD2,
  HL_STRING,
  HL_NUMBER,
  HL_MATCH 
};

/* bit flags */ 
#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** data ***/

struct editorSyntax {
  /* the name of the filetype that will be displayed */ 
  char *filetype;
  /* an array of strings where each string contains a */
  /* pattern to match a filename against */ 
  char **filematch;
  char **keywords;
  /* define what a singleline comment starts with */ 
  char *singleline_comment_start;
  char *multiline_comment_start;
  char *multiline_comment_end;
  /* will contain flags for whether to hgighlight numbers etc */ 
  int flags;
};

/* editor row, store a line of text and its length; */
typedef struct erow {
  int idx;
  int size;
  int rsize;
  char *chars;
  char *render;
  /* unsigned char will mean integers in the range 0 to 255 */ 
  unsigned char *hl; 
  int hl_open_comment;
} erow;

struct editorConfig {
  /* cx is an index in the chars filed of an erow, rx will be an index into render */ 
  /* if there are no tabs on the line then rx will be the same as cx */
  int cx, cy;
  int rx;
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  int dirty;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  /* 0 is standard, 1 is editing */ 
  int editorMode;
  struct editorSyntax *syntax;
  struct termios orig_termios;
};

struct editorConfig E;

/*** filetypes ***/

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };
/* marking the second type with a '|' character */ 
char *C_HL_keywords[] = {
  "switch", "if", "while", "for", "break", "continue", "return", "else",
  "struct", "union", "typedef", "static", "enum", "class", "case",
  "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
  "void|", NULL
};

/* highlight database */ 
struct editorSyntax HLDB[] = {
  {
    "c", 
    C_HL_extensions,
    C_HL_keywords,
    "//", "/*", "*/",
    HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
  },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

void die(const char *s) {
  /* print error messages when libraries fail */

  /* clear the screen before exiting */ 
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void disableRawMode() {
  /* disable raw mode in the terminal */ 
  if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  /* enable raw mode in the terminal which means input won't be echoed out */ 

  /* store the original terminal attributes so we can bring them back on exit */  
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");

  /* will be run when returning from main or when exit function called */ 
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;

  /* ECHO is a bitflag that causes every key you type to be printed to the terminal */
  /* ICANON AND ISIG disable ctrl-c and ctrl-z */
  /* IEXTEN disables ctrl-v */
  /* Here we use bitwise operators to mark it as 'false' */
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); 

  /* IXON disables ctrl-s and ctrl-q (stopping and starting input) */ 
  /* ICRNL disables the conversion of carriage return (CR) into newlines (NL) */
  raw.c_lflag &= ~(ICRNL | IXON); 

  /* turns off all output processing */ 
  raw.c_lflag &= ~(OPOST); 

  /* these are some miscellaneous flags for old emulators etc for enabling raw mode */  
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); 
  raw.c_lflag |= ~(CS8); 

  /* adds a one hundred milisecond time out */ 
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
  /* handles keypresses */ 

  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch(seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch(seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return ARROW_RIGHT;
          case 'F': return ARROW_LEFT;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }
    
    return '\x1b';
  } else {
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  /* check that it returns an escape sequence */
  if (buf[0] != '\x1b' || buf[1] != '[') return -1;

  /* parse the 2 numbers we need out of the escape sequence */
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
} 

int getWindowSize(int *rows, int *cols) {
  /* returns the current window size (rows and columns) */
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    /* there has been an error and either ioctl() has returned -1 (error) or the number of cols was 0 */

    /* use C and B commands to find the edge of the screen */
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);

  } else {
    /* success! set number of columns and rows */ 
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** syntax highlighting ***/

int is_separator(int c) {
  /* return true if c is a space, a return character or one of */ 
  /* the characters in that string */ 
  return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
  /* realloc the needed memory */ 
  /* since it oculd be bigger than the last */
  /* time we highlighted it */ 
  row->hl = realloc(row->hl, row->rsize);

  /* use memset to set all the characters to */ 
  /* HL_NORMAL */ 
  memset(row->hl, HL_NORMAL, row->rsize);

  if (E.syntax == NULL) return;

  /* make keywords a shortcut for this */ 
  char **keywords = E.syntax->keywords;

  /* make aliases to make the code shorter */ 
  char *scs = E.syntax->singleline_comment_start;
  char *mcs = E.syntax->multiline_comment_start;
  char *mce = E.syntax->multiline_comment_end;

  int scs_len = scs ? strlen(scs) : 0;
  int mcs_len = mcs ? strlen(mcs) : 0;
  int mce_len = mce ? strlen(mce) : 0;

  /* initialised to true */ 
  /* because beginning of line */
  /* is considered a separator */
  int prev_sep = 1;

  /* keep track if we are currently inside */
  /* a string or a comment */ 
  int in_string = 0;
  int in_comment = (row->idx > 0 && E.row[row->idx-1].hl_open_comment);

  int i = 0;;
  while (i < row->rsize) {
    char c = row->render[i];
    unsigned char prev_hl = (i > 0) ? row->hl[i-1] : HL_NORMAL;

    /* make sure we are not in a string and there is a commaent */ 
    if (scs_len && !in_string && !in_comment) {
      /* use strncmp to see if this is the start of a new line */ 
      if (!strncmp(&row->render[i], scs, scs_len)) {
        /* if so, set the colour with memset */
        memset(&row->hl[i], HL_COMMENT, row->rsize - i);
        break;
      }
    }

    /* check there are non null strings and we are not in a string */ 
    if (mcs_len && mce_len && !in_string) {
      /* if already in a comment */ 
      if (in_comment) {
        row->hl[i] = HL_MLCOMMENT;
        /* if we are at the end */ 
        if (!strncmp(&row->render[i], mce, mce_len)) {
          /* reset colour */ 
          memset(&row->hl[i], HL_MLCOMMENT, mce_len);
          i += mce_len;
          in_comment = 0;
          prev_sep = 1;
          continue;
        } else {
          /* otherwise move on */ 
          i++;
          continue;
        }
      } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
        memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
        i += mcs_len;
        in_comment = 1;
        continue;
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
      if (in_string) {
        row->hl[i] = HL_STRING;

        /* if we are in a string */ 
        /* and the current character is a backslash */
        /* and there is at least one more character in that line */ 
        /* after the backslash then we highlight the character that */ 
        /* came before and consume it, we increment i by 2 to consume both */ 
        /* at one */ 
        if (c == '\\' && i + 1 < row->rsize) {
            row->hl[i] = HL_STRING;
            i += 2;
            continue;
        }

        if (c == in_string) in_string = 0;
        i++;
        prev_sep = 1;
        continue;
      } else {
        if (c == '"' || c == '\'') {
          in_string = c;
          row->hl[i] = HL_STRING;
          i++;
          continue;
        }
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
      if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c == '.' && prev_hl == HL_NUMBER)) {
        row->hl[i] = HL_NUMBER;
        i++;

        /* indicates we are in the process of highlighting */ 
        prev_sep = 0;
        continue;
      }
    } 

    if (prev_sep) {
      int j;
      for (j = 0; keywords[j]; j++) {
        int klen = strlen(keywords[j]);
        int kw2 = keywords[j][klen - 1] == '|';
        if (kw2) klen--;

        if (!strncmp(&row->render[i], keywords[j], klen) && is_separator(row->render[i + klen])) {
          memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
          i += klen;
          break;
        }
      }
      if (keywords[j] != NULL) {
        prev_sep = 0;
        continue;
      }
    }

    prev_sep = is_separator(c);
    i++;
  }


  /* if comment state has changed */ 
  int changed = (row->hl_open_comment != in_comment);
  /* change state to whatever in_comment is */ 
  row->hl_open_comment = in_comment;

  /* update the syntax of every line below because */ 
  /* you could feasibly comment out the entire file */ 
  if (changed && row->idx + 1 < E.numrows)
    editorUpdateSyntax(&E.row[row->idx + 1]);
}

int editorSyntaxToColour(int hl) {
  /* return integer ansi codes for the colours */
  /* of the syntax parts */ 

  switch (hl) {
    case HL_COMMENT: 
    case HL_MLCOMMENT: return 36;
    case HL_KEYWORD1: return 33;
    case HL_KEYWORD2: return 32;
    case HL_STRING: return 35;
    case HL_NUMBER: return 31;
    case HL_MATCH: return 34;
    default: return 37;
  }
}

void editorSelectSyntaxHighlight() {
  /* default is null */ 
  /* if there are no matches and no file name we have no filetype */ 
  E.syntax = NULL;
  if (E.filename == NULL) return;

  /* pointer to the extension part of the filename */ 
  char *ext = strrchr(E.filename, '.');

  /* loop through each struct */ 
  /* if the pattern is not in there */ 
  /* then don't use it for anything */ 
  for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
    struct editorSyntax *s = &HLDB[j];
    unsigned int i = 0;
    while (s->filematch[i]) {
      int is_ext = (s->filematch[i][0] == '.');
      if ((is_ext && ext && !strcmp(ext, s->filematch[i])) || (!is_ext && strstr(E.filename, s->filematch[i]))) {
        E.syntax = s;

        /* update highlighting when syntax set */ 
        int filerow;
        for (filerow=0; filerow < E.numrows; filerow++) {
          editorUpdateSyntax(&E.row[filerow]);
        }

        return;
      }
      i++;
    }
  }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    /* if the charater is a tab */
    if (row->chars[j] == '\t') 
      /* calculate how many columns we are to the right of the last tab stop */ 
      /* and subtract that from EDITOR_TAB_STOP-1 to find out how many columns */
      /* to the left of the next tab stop we are */ 
      rx += (EDITOR_TAB_STOP - 1) - (rx % EDITOR_TAB_STOP);
    rx++;
  }
  return rx;
}

int editorRowRxToCx(erow *row, int rx) {
  /* loop through the chars string */ 
  /* calculating the current rx value */ 
  /* stopping when cur_rx hits the given rx value */
  /* and return cx */
  int cur_rx = 0;
  int cx;
  for (cx = 0; cx < row->size; cx++) {
    if (row->chars[cx] == '\t')
      cur_rx += (EDITOR_TAB_STOP-1) - (cur_rx % EDITOR_TAB_STOP);
    cur_rx++;

    if (cur_rx > rx) return cx;
  }
  return cx;
}

void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  /* loop through the chars in the row and count the tabs */ 
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;

  /* allocate the appropriate amount of memory for the tabs */ 
  free(row->render);
  row->render = malloc(row->size + tabs*(EDITOR_TAB_STOP-1) + 1);

  /* for loop checks if a character is a tab */ 
  /* if it is it will add spaces */ 
  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % EDITOR_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }

  row->render[idx] = '\0';
  row->rsize = idx;

  editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;

  /* this allocates an amount of memory that is the number of rows + 1 */
  /* times the number of rows we want plus one */ 
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  /* update idx of each row when we insert row */ 
  for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;

  /* intialise idx to the rows index */
  /* in the file when it is inserted */
  E.row[at].idx = at;

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  E.row[at].hl = NULL;
  E.row[at].hl_open_comment = 0;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
  free(row->hl);
}

void editorDelRow(int at) {
  /* validate index */ 
  if (at < 0 || at >= E.numrows) return;

  /* delete row and move up */ 
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at+1], sizeof(erow) * (E.numrows - at - 1));

  /* update indexes when deleting */ 
  for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;

  /* update everything */ 
  E.numrows--;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  /* row is the row we are working on */ 
  /* at is the index we want to insert the character into */ 
  /* c is the character we want to insert */ 

  /* if at is out of bounds */ 
  /* we make it the last character of the row */ 
  if (at < 0 || at > row->size) at = row->size;

  /* add one more byte for the char to the erow */
  /* we add two because we are adding one for the null byte */ 
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at+1], &row->chars[at], row->size - at + 1);
  
  /* increment size, because we now have one more character */
  row->size++;

  /* finally assign the character to its index and update */ 
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  /* the new size is size + the append + the null byte */
  row->chars = realloc(row->chars, row->size + len + 1);

  /* copy the given string to the end of row->chars */
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';

  /* update everything */
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  /* if error, just return */
  if (at < 0 || at >= row->size) return;

  /* use memmove() to overwrite the deleted character with */ 
  /* the characters before it */ 
  memmove(&row->chars[at], &row->chars[at+1], row->size - at);

  /* update everything */ 
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

/*** editor operations **/
void editorInsertChar(int c) {
  /* if we are at the bottom of the document */ 
  if (E.cy == E.numrows) {
    /* add a new line */ 
    editorInsertRow(E.numrows, "", 0);
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewline() {
  if (E.cx == 0) {
    /* if we're at the beginning of a line */ 
    /* insert a new blank row before the line we are on */ 
    editorInsertRow(E.cy, "", 0);
  } else {
    /* otherwise split into two rows */
    erow *row = &E.row[E.cy];
    /* pass characters on the right of the cursor to new row */ 
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);

    /* reassign the pointer */ 
    /* because realoc might move around memory and invalidate */
    row = &E.row[E.cy];

    /* truncate the current row contents */
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }

  /* move the cursor down one and back to the left */ 
  E.cy++;
  E.cx = 0;
}
void editorDelChar() {
  /* if the cursor is past the end of the file, there is nothing to delete */ 
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;

  /* go to the row the cursor is on */ 
  erow *row = &E.row[E.cy];

  /* if there is a character left of the cursor */ 
  if (E.cx > 0) {
    /* delete */ 
    editorRowDelChar(row, E.cx - 1);
    /* move one to the left */ 
    E.cx--;
  } else {
    E.cx = E.row[E.cy-1].size;
    editorRowAppendString(&E.row[E.cy-1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;

  /* add up all the lengths (including +1 for the newlines) */ 
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;

  /* allocate the required memory */ 
  char *buf = malloc(totlen);
  char *p = buf;

  /* loop through the rows and memcpy the contents of each row */ 
  /* to the end of the buffer appending a newline character after */
  /* each row */ 
  for ( j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }

  /* return buf, expecting the caller to free the memory */
  return buf;
}

void editorOpen(char *filename) {
  /* take a copy of a string and allocate the memory for it */ 
  free(E.filename);
  E.filename = strdup(filename);

  editorSelectSyntaxHighlight();

  /* takes a filename and 'opens' it by counting the lines and extracting them into buffer */ 
  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  /* get the line and linelength values */ 
  /* we pass a null pointer and line capacity of 0 */  
  /* this makes it allocate memory for the next line it reads */ 
  /* getline returns the length of the line it read */ 
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;
    
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  /* if it's a new file */ 
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
    editorSelectSyntaxHighlight();
  }

  int len;
  char *buf = editorRowsToString(&len);

  /* O_RDWR : we want to open for reading and writing if it does exist */ 
  /* O_CREAT : we want to create a new file if it doesn't exist */ 
  /* 0644 means the owner of the file has permission to read and write and every */
  /* one else has permission just to read */ 
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) != -1) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
  }

  /* TODO */ 
  /* create temporary files to store changes instead of truncating */
  /* /writing to the file as it is dangerous */ 
  /* ftruncate(fd, len); */
  /* write(fd, buf, len); */
  /* close(fd); */
  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editorFindCallback(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;

  /* using static variables to know */ 
  /* which lines hl needs to be restored */ 
  static int saved_hl_line;
  static char *saved_hl = NULL;

  if (saved_hl) {
    memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
    free(saved_hl);
    saved_hl = NULL;
  }
  /* always reset last_match to -q unless arrow pressed */ 
  /* if key is enter or escape, leave search by resetting last_match and direction */
  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP) {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }

  /* create prompt */ 
  if (last_match == -1) direction = 1;
  int current = last_match;

  /* loop through all rows of the flie */
  /* use strstr to check if query is */ 
  /* a substring of the current row */
  int i;
  for (i = 0; i < E.numrows; i++) {
    current += direction;
    if (current == -1) current = E.numrows - 1;
    else if (current == E.numrows) current = 0;

    erow *row = &E.row[current];
    char *match = strstr(row->render, query);
    if (match) {
      /* change last_match to the index of the current row we are searching */ 
      /* so if user presses arrow keys, we'll start search from there */
      last_match = current;
      /* move the cursor to that position */
      E.cy = current;
      E.cx = editorRowRxToCx(row, match - row->render);
      E.rowoff = E.numrows;

      saved_hl_line = current;
      saved_hl = malloc(row->rsize);
      memcpy(saved_hl, row->hl, row->rsize);

      /* highlight the searched value */ 
      /* match - row->render is the index */ 
      /* into render of the match */ 
      memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
      break;
    }
  }
}

void editorFind() {
  /* save positions so we can reset if user exits search */ 
  int saved_cx = E.cx;
  int saved_cy = E.cy;
  int saved_coloff = E.coloff;
  int saved_rowoff = E.rowoff;

  /* if escape pressed */ 
  /* call query with callback function */ 
  char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

  /* if query is not null they pressed esc */ 
  /* so we restore the previous position */ 
  if (query) {
    free(query);
  } else {
    E.cx = saved_cx;
    E.cy = saved_cy;
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;
  }
}

/*** append buffer ***/

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  /* ask for a black of memory that is the size of the current string + the appended string */ 
  char *new = realloc(ab->b, ab->len + len);

  /* if we have nothing to add, return */ 
  if (new == NULL) return;

  /* copy the string and update the point and length */
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  /* a destructor that deallocates the dynamic memory */
  free(ab->b);
}

/*** input ***/

/* passing a callback function for search, pass NULL to avoid */ 
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);

  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    /* repeatedly set the status message */ 
    /* refresh the screen and read for input */ 
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey();
    if (c == DEL_KEY || c ==  CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      /* if escape pressed */ 
      /* empty status and free memory */ 
      editorSetStatusMessage("");
      if (callback) callback(buf, c);
      free(buf);
      return NULL;
    } else if (c == '\r') {
      /* when the user presses enter */ 
      /* return the input if length is not 0 */ 
      if (buflen != 0) {
        editorSetStatusMessage("");
        if (callback) callback(buf, c);
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      /* if c is not a control character */ 
      /* if buflen gets too big we duplicate */ 
      /* bufsize and we allocate the amount */
      /* and make sure it ends with the null char */
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      } 
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }

    if (callback) callback(buf, c);
  }
}

void editorMoveCursor(int key) {
  /* check if the cursor is on the actual line */ 
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  /* change the cursor x and y values (convention is 0 at the top, 0 at the left) */
  switch (key) {
    case ARROW_LEFT:
    case 'h':
      if (E.cx != 0) {
        E.cx--;
        /* check we are not at the top of the document */ 
      } else if (E.cy > 0) {
        /* if not, move up a line */ 
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
    case 'l':
      /* limits scrolling to the right */ 
      if (row && E.cx < row->size) {
        E.cx++;
      } else if (row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
    case 'k':
      if (E.cy != 0) {
        E.cy--;
      }
      break;
    case ARROW_DOWN:
    case 'j':
      if (E.cy < E.numrows) {
        E.cy++;
      }
      break;
  }

  /* snapping the cursor to the end of the line */ 

  /* set row again */ 
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  /* move the cursor if it is beyond the end of the line */
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

void editorProcessKeypress() {
  /* handles keypresses with switch statement */ 
  static int quit_times = EDITOR_QUIT_TIMES;

  int c = editorReadKey();

  switch(c) {
    /* quits */ 
    case CTRL_KEY('q'): 
      if (E.dirty && quit_times > 0) {
        /* send quit message */ 
        editorSetStatusMessage("WARNING!!! File has unsaved changes. Press ctrl-q %d more times to quit.", quit_times);

        quit_times--;
        return;
      }
      /* clear the screen before exiting */ 
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);

      exit(0);
      break;

    case CTRL_KEY('s'):
      /* save file */ 
      editorSave();
      break;

    case HOME_KEY:
      /* move cursor to the start of the line */ 
      E.cx = 0;
      break;

    case END_KEY:
      /* move cursor to the end (length of the line) */ 
      if (E.cy < E.numrows)
        E.cx = E.row[E.cy].size;
      break;

    case CTRL_KEY('f'):
      editorFind();
      break;

    case CTRL_KEY('e'): 
    case CTRL_KEY('y'): 
    case PAGE_UP:
    case PAGE_DOWN:
      /* we implement page up and down by running up and down enough times to move a page */
      {
        if (c == PAGE_UP || c == CTRL_KEY('y')) {
          /* position cursor at the top of the page */
          E.cy = E.rowoff;
        } else if (c == PAGE_DOWN || c == CTRL_KEY('e')) {
          /* position cursor at the bottom of the page */ 
          E.cy = E.rowoff + E.screenrows - 1;
          if (E.cy > E.numrows) E.cy = E.numrows;
        }

        int times = E.screenrows;
        while (times--)
          editorMoveCursor(c == PAGE_UP || c == CTRL_KEY('y') ? ARROW_UP : ARROW_DOWN);
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;

    /* refresh and escape */ 
    case CTRL_KEY('l'):
    case '\x1b':
      break;

    default:
      if (E.editorMode == 1) {
        switch(c) {
          case '\r':
            editorInsertNewline();
            break;

          case BACKSPACE:
          case CTRL_KEY('h'):
          case DEL_KEY:
            /* handle the delete key by backspacing and moving right */ 
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

          /* ctrl-j to enter insert mode */ 
          case CTRL_KEY('j'):
            E.editorMode = 0;
            break;

          default: 
            editorInsertChar(c);
        }
      } else {
        switch(c) {
          case 'h':
          case 'j':
          case 'k':
          case 'l':
            editorMoveCursor(c);
            break;

          case 'i':
            E.editorMode = 1;
            break;

          /* case '\r': */
          /*   editorInsertNewline(); */
          /*   E.cy++; */
          /*   E.editorMode = 1; */
          /*   break; */
        }
      }
    }

  quit_times = EDITOR_QUIT_TIMES;
}

/*** output ***/

void editorScroll() {
  E.rx = E.cx; 

  /* check if the cursor is above the window */ 
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  /* check if the cursor is below the window */ 
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  /* check if the cursor is beyond the column offset */ 
  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  /* draw a tilde at the start of rows */ 
  int y;
  for (y = 0; y < E.screenrows; y++) {
    /* calculate our position in the file with the new offset */ 
    int filerow = y + E.rowoff;

    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        /* if there are no rows (no file) then print welcome screen */ 
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),"editor -- version %s", EDITOR_VERSION);

        if (welcomelen > E.screencols) welcomelen = E.screencols;

        /* divide the screeen in 2 and subtract half the string length */ 
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[filerow].rsize - E.coloff;

      /* if len goes negative we restrict it to zero */ 
      if (len < 0) len = 0;

      /* if it goes above the number of columns, we set it back to the max */
      if (len > E.screencols) len = E.screencols;

      /* feeding render to abAppend character by character */ 
      /* loop through the characters and precede it with the */ 
      /* correct commmand to set colour */ 

      /* pointer to the slice of the hl array that */ 
      /* is the slice of render we are printing */ 
      char *c = &E.row[filerow].render[E.coloff];
      unsigned char *hl = &E.row[filerow].hl[E.coloff];
      int current_colour = -1;
      int j;
      for (j=0; j<len; j++) {
        /* check if is control character */ 
        /* turn it into a printable character */ 
        /* by adding to at symbol */ 
        /* or use ? if not in alphabetical range */ 
        /* then use <esc>[7m to switch to inverted colours */
        if (iscntrl(c[j])) {
            char sym = (c[j] <= 26 ? '@' + c[j] : '?');
            abAppend(ab, "\x1b[7m", 4);
            abAppend(ab, &sym, 1);
            abAppend(ab, "\x1b[m", 3);
            /* return back to colour after inverting */ 
            if (current_colour != -1) {
              char buf[16];
              int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_colour);
              abAppend(ab, buf, clen);
            }
          } else if (hl[j] == HL_NORMAL) {
          /* the m command handles colour things */
          /* from 30 - 37, 0 is black, 1 is red etc */
          if (current_colour != -1) {
            abAppend(ab, "\x1b[39m", 5);
            current_colour = -1;
          }
          abAppend(ab, &c[j], 1);
        } else {
          /* if it isn't normal we print with snprintf */ 
          /* the colour returned by editorSyntaxToColour */
          int colour = editorSyntaxToColour(hl[j]);
          if (colour != current_colour) {
            current_colour = colour;
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", colour);
            abAppend(ab, buf, clen);
          }
          abAppend(ab, &c[j], 1);
        }
      }
      /* return to white */ 
      abAppend(ab, "\x1b[39m", 5);
    }

    /* erases everything right of the cursor */
    /* clearing everything as we redraw */ 
    abAppend(ab, "\x1b[K", 3);

    /* draw a newline after ever line */ 
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  /* inverts colours using m command */ 
  abAppend(ab, "\x1b[7m", 4);

  /* declare the status bar strings */ 
  char status[80], rstatus[80];

  /* declares the left side status string */ 
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s - %s", E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "", E.editorMode ? "ed" : "st");

  /* declares the right side status string */ 
  int rlen = snprintf(rstatus, sizeof(status), "%s | %d/%d", E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.numrows);

  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);

  while(len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }

  /* goes back to normal formatting */ 
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  /* clear the message bar with K command */ 
  abAppend(ab, "\x1b[K", 3);

  /* make sure the message will fit the bar */ 
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;

  /* display the message but only if it is less than 5 seconds young */ 
  if (msglen && time(NULL) - E.statusmsg_time < 5) 
    abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
  /* write is a function provided by <unistd.h> */
  /* here the 4 means we are writing 4 bytes to the terminal */
  /* the first byte is \x1b, this is the escape character */ 
  /* the following three are [2J */  
  /* we are using the J command (erase in display) with */
  /* the argument 2 which means clear the entire screen */

  /* check if we need to scroll through the file */ 
  /* (cursor moving above or below) */
  editorScroll();

  struct abuf ab = ABUF_INIT;

  /* hiding the cursor */ 
  abAppend(&ab, "\x1b[?25l", 6);

  /* this is a 3 byte sequence */ 
  /* using the H (cursor position) command */ 
  /* we are using the default arguments for H so */
  /* the cursor is positioned at 1,1 */
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  /* move the cursor */ 
  char buf[32];

  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  /* bringing back the cursor */ 
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  /* the ... makes the function 'variadic' */ 
  /* meaning it can take any number of arguments */ 
  va_list ap;
  va_start(ap, fmt);
  
  /* making our own 'printf style function' */ 
  /* we store the output string in E.statusmsg */
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);

  va_end(ap);

  /* then we set the time to the current time */ 
  /* this is what you get when you pass time() NULL */
  E.statusmsg_time = time(NULL);
}

/*** init ***/
void initEditor() {
  E.cx = 0; 
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.syntax = NULL;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");

  /* make room for the status bar */ 
  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  /* enable 'raw mode' which lets us read every character individually */ 
  enableRawMode(); 
  /* sets inital conditions (windowsize, cursor position etc) */
  initEditor();
  /* opens file or home */ 
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: ctrl-s = save | ctrl-q = quit | ctrl-f = find");

  /* run loop that refreshes the screen and reads keypresses */ 
  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
