# Passwordless sudo for user `tony`

This guide shows how to let the user `tony` run `sudo` without being prompted for a password.

## Recommended method: a drop-in file in `/etc/sudoers.d/`

1. Create a new file with `visudo` (it performs syntax checking and prevents broken sudoers files):

   ```sh
   sudo visudo -f /etc/sudoers.d/tony
   ```

2. Add this line:

   ```
   tony ALL=(ALL) NOPASSWD: ALL
   ```

3. Save and exit. `visudo` will warn you about syntax errors before writing the file.

4. Set the correct permissions (usually done automatically by `visudo`):

   ```sh
   sudo chmod 440 /etc/sudoers.d/tony
   ```

The change takes effect immediately — no service restart needed. Test it with:

```sh
sudo -l          # should list "(ALL) NOPASSWD: ALL"
sudo whoami      # should print "root" without asking for a password
```

## Line breakdown

| Part        | Meaning                                                                 |
| ----------- | ----------------------------------------------------------------------- |
| `tony`      | The user the rule applies to                                            |
| `ALL`       | Applies from any host (relevant when sudoers is shared across machines) |
| `(ALL)`     | May run commands as any user (and any group with `(ALL:ALL)`)           |
| `NOPASSWD:` | Do not prompt for a password                                            |
| `ALL`       | May run any command                                                     |

## Alternative: edit the main sudoers file

Run `sudo visudo` and add the same line under the existing `%sudo` / `%wheel` entry:

```
tony ALL=(ALL) NOPASSWD: ALL
```

Always edit sudoers through `visudo` — a syntax error in `/etc/sudoers` can lock you out of sudo entirely.

## Restricting to specific commands (optional)

Instead of granting full access, you can limit passwordless sudo to specific commands:

```
tony ALL=(ALL) NOPASSWD: /usr/bin/apt, /usr/bin/systemctl
```

## Notes and caveats

- **Security trade-off:** passwordless sudo means anyone with access to `tony`'s session has full root access without further authentication. Only do this on trusted, single-user machines (e.g. a personal dev box or VM).
- **Group variant:** to give all members of the `sudo` group passwordless sudo, uncomment this line in `/etc/sudoers`:

  ```
  %sudo ALL=(ALL:ALL) NOPASSWD: ALL
  ```

  (On some distros the group is `%wheel` instead.)
- **Files in `/etc/sudoers.d/` must not contain `.` or `~` in their names** and should not be edited with regular editors directly, or `visudo`/sudo may ignore them.
- If changes don't seem to apply, check that `/etc/sudoers` contains `@includedir /etc/sudoers.d` (at the end of the file) and that no later entry overrides the rule.
