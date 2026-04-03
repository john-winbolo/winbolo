#ifndef SENTRY_INTEGRATION_H
#define SENTRY_INTEGRATION_H

int sentryInit(const char *executable_name, int argc, char *argv[]);
void sentryClose(void);

#endif
