/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#ifndef __CLEARHPGL_CONSOLE_H__
#define __CLEARHPGL_CONSOLE_H__

/*
 * Line-oriented setup and status console. The same commands run on USB (after
 * "menu" + Enter) and on TCP 9300. Replies are plain lines; a command's last
 * line starts with "ok" or "error:" so host tools can parse it.
 */

typedef void (*ConsoleOut)(const char *line);

void ConsoleBanner(ConsoleOut out);
/* Returns true when the session should end ("exit"). */
bool ConsoleHandleLine(const char *line, ConsoleOut out);

#endif
