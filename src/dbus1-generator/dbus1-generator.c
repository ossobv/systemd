/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include "util.h"
#include "conf-parser.h"
#include "special.h"
#include "mkdir.h"
#include "bus-util.h"
#include "bus-internal.h"
#include "unit-name.h"
#include "cgroup-util.h"

static const char *arg_dest = "/tmp";

static int create_dbus_files(
                const char *path,
                const char *name,
                const char *service,
                const char *exec,
                const char *user,
                const char *type) {

        _cleanup_free_ char *b = NULL, *s = NULL, *lnk = NULL;
        _cleanup_fclose_ FILE *f = NULL;

        assert(path);
        assert(name);

        if (!service) {
                _cleanup_free_ char *a = NULL;

                s = strjoin("dbus-", name, ".service", NULL);
                if (!s)
                        return log_oom();

                a = strjoin(arg_dest, "/", s, NULL);
                if (!a)
                        return log_oom();

                f = fopen(a, "wxe");
                if (!f) {
                        log_error("Failed to create %s: %m", a);
                        return -errno;
                }

                fprintf(f,
                        "# Automatically generated by systemd-dbus1-generator\n\n"
                        "[Unit]\n"
                        "Source=%s\n"
                        "Description=DBUS1: %s\n\n"
                        "[Service]\n"
                        "ExecStart=%s\n"
                        "Type=dbus\n"
                        "BusName=%s\n",
                        path,
                        name,
                        exec,
                        name);

                if (user)
                        fprintf(f, "User=%s\n", user);


                if (type) {
                        fprintf(f, "Environment=DBUS_STARTER_BUS_TYPE=%s\n", type);

                        if (streq(type, "system"))
                                fprintf(f, "Environment=DBUS_STARTER_ADDRESS=kernel:/dev/kdbus/0-system\n");
                        else if (streq(type, "session"))
                                fprintf(f, "Environment=DBUS_STARTER_ADDRESS=kernel:/dev/kdbus/%lu-user\n", (unsigned long) getuid());
                }

                fflush(f);
                if (ferror(f)) {
                        log_error("Failed to write %s: %m", a);
                        return -errno;
                }

                service = s;
        }

        b = strjoin(arg_dest, "/", name, ".busname", NULL);
        if (!b)
                return log_oom();

        f = fopen(b, "wxe");
        if (!f) {
                log_error("Failed to create %s: %m", b);
                return -errno;
        }

        fprintf(f,
                "# Automatically generated by systemd-dbus1-generator\n\n"
                "[Unit]\n"
                "Source=%s\n"
                "Description=DBUS1: %s\n\n"
                "[BusName]\n"
                "Name=%s\n"
                "Service=%s\n",
                path,
                name,
                name,
                service);

        fflush(f);
        if (ferror(f)) {
                log_error("Failed to write %s: %m", b);
                return -errno;
        }

        lnk = strjoin(arg_dest, "/" SPECIAL_BUSNAMES_TARGET ".wants/", name, ".busname", NULL);
        if (!lnk)
                return log_oom();

        mkdir_parents_label(lnk, 0755);
        if (symlink(b, lnk)) {
                log_error("Failed to create symlinks %s: %m", lnk);
                return -errno;
        }

        return 0;
}

static int add_dbus(const char *path, const char *fname, const char *type) {
        _cleanup_free_ char *name = NULL, *exec = NULL, *user = NULL, *service = NULL;

        ConfigTableItem table[] = {
                { "D-BUS Service", "Name", config_parse_string, 0, &name },
                { "D-BUS Service", "Exec", config_parse_string, 0, &exec },
                { "D-BUS Service", "User", config_parse_string, 0, &user },
                { "D-BUS Service", "SystemdService", config_parse_string, 0, &service },
        };

        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *p = NULL;
        int r;

        assert(path);
        assert(fname);

        p = strjoin(path, "/", fname, NULL);
        if (!p)
                return log_oom();

        f = fopen(p, "re");
        if (!f) {
                if (errno == -ENOENT)
                        return 0;

                log_error("Failed to read %s: %m", p);
                return -errno;
        }

        r = config_parse(NULL, p, f, "D-BUS Service\0", config_item_table_lookup, table, true, false, NULL);
        if (r < 0)
                return r;

        if (!name) {
                log_warning("Activation file %s lacks name setting, ignoring.", p);
                return 0;
        }

        if (!service_name_is_valid(name)) {
                log_warning("Bus service name %s is not valid, ignoring.", name);
                return 0;
        }

        if (streq(name, "org.freedesktop.systemd1")) {
                log_debug("Skipping %s, identified as systemd.", p);
                return 0;
        }

        if (service) {
                if (!unit_name_is_valid(service, false)) {
                        log_warning("Unit name %s is not valid, ignoring.", service);
                        return 0;
                }
                if (!endswith(service, ".service")) {
                        log_warning("Bus names can only activate services, ignoring %s.", p);
                        return 0;
                }
        } else {
                if (streq(exec, "/bin/false") || !exec) {
                        log_warning("Neither service name nor binary path specified, ignoring %s.", p);
                        return 0;
                }

                if (exec[0] != '/') {
                        log_warning("Exec= in %s does not start with an absolute path, ignoring.", p);
                        return 0;
                }
        }

        return create_dbus_files(p, name, service, exec, user, type);
}

static int parse_dbus_fragments(void) {
        _cleanup_closedir_ DIR *d = NULL;
        struct dirent *de;
        const char *p, *type;
        int r;

        r = cg_pid_get_owner_uid(0, NULL);
        if (r >= 0) {
                p = "/usr/share/dbus-1/services";
                type = "session";
        } else if (r == -ENOENT) {
                p = "/usr/share/dbus-1/system-services";
                type = "system";
        } else if (r < 0) {
                log_error("Failed to determine whether we are running as user or system instance: %s", strerror(-r));
                return r;
        }

        d = opendir(p);
        if (!d) {
                if (errno == -ENOENT)
                        return 0;

                log_error("Failed to enumerate D-Bus activated services: %m");
                return -errno;
        }

        r = 0;
        FOREACH_DIRENT(de, d, goto fail) {
                int q;

                if (!endswith(de->d_name, ".service"))
                        continue;

                q = add_dbus(p, de->d_name, type);
                if (q < 0)
                        r = q;
        }

        return r;

fail:
        log_error("Failed to read D-Bus services directory: %m");
        return -errno;
}

int main(int argc, char *argv[]) {
        int r;

        if (argc > 1 && argc != 4) {
                log_error("This program takes three or no arguments.");
                return EXIT_FAILURE;
        }

        if (argc > 1)
                arg_dest = argv[3];

        log_set_target(LOG_TARGET_SAFE);
        log_parse_environment();
        log_open();

        umask(0022);

        if (access("/dev/kdbus/control", F_OK) < 0)
                return 0;

        r = parse_dbus_fragments();

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
