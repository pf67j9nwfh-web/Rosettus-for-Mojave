# Adobe Version Cue CS2 background service

Experimental support runs the installed Adobe Version Cue CS2 server on Mojave through Rosettus. The native Rosettus Preference Pane Viewer discovers the installed PowerPC pane automatically and also offers Other… for manual selection. With the service running, Adobe's original pane shows On and enables its controls.

## Install and control

Install Adobe Version Cue CS2 and Rosettus's optional exported PowerPC Java 5 first. From the Rosettus repository on Mojave:

```
sudo /usr/bin/python tools/install_versioncue.py --root /Library/Rosettus
```

A system Rosettus install also configures this optional service when the Adobe preferences, Java 5 export, and support binaries are present, unless `--no-agents` was supplied. Per-user installs copy support files but do not install a system daemon.

The installer reads Adobe's existing VersionCuePath and workspace settings. It copies Adobe's launcher into the runtime and changes only that copy's JavaVM dependency. Adobe binaries and Apple Java files are not distributed in the repository. Back up the existing Adobe configuration and data before using an important workspace.

The system LaunchDaemon is `/Library/LaunchDaemons/io.github.rosettus.versioncue.plist`. Its controller runs as root, matching the installed service's permission requirements; Adobe's database drops to its own account. The existing AutoStart preference determines whether the server starts when the controller loads. The controller remains available for pane start requests.

```
/Library/Rosettus/versioncue/versioncue status
/Library/Rosettus/versioncue/versioncue start
/Library/Rosettus/versioncue/versioncue stop
/Library/Rosettus/versioncue/versioncue restart
```

These commands use Adobe's original service/status protocol and can be run by the login user. The original pane is the GUI for these controls. The installed configuration's administration page is at `http://localhost:3703/`. Logs are in `/Library/Logs/Rosettus/VersionCue-service.log` and Adobe's existing `logs` directory.

To remove the service, allowing the server and database to finish shutdown:

```
sudo /usr/bin/python tools/install_versioncue.py --root /Library/Rosettus --remove
```

Rosettus uninstall invokes the same drain procedure before removing the runtime. If the server is still shutting down after 90 s (`VC_STOP_TIMEOUT` in the service's environment), the controller ends the server and its database (the server's process group) so the service can be started again. The helper waits up to 150 s for the controller to exit, ends it if it has not, and always starts the service again if an install fails part-way. Existing Adobe workspace files remain in place.

## Implementation

A PowerPC controller implements Adobe's `.ctrl` CFMessagePort and talks to the server's original `.status` port. Using a PowerPC controller preserves the old framework's message encoding. It launches the copied native Adobe service executable, retaining the native Java methods and status behavior expected by the pane.

A small JavaVM shim loads the exported Java 5 implementation and adds interpreter mode, headless mode, and a Java instrumentation agent. Interpreter mode avoids relying on unsupported JIT fault behavior. The agent adds explicit null checks to newly loaded application bytecode so normal Java NullPointerExceptions do not depend on forwarding guest memory faults through Rosettus. It preserves existing catch handlers and avoids uninitialized constructor receivers. This is a targeted compatibility measure, not complete JVM signal support; bootstrap classes are deliberately excluded.

During shutdown, the controller waits for Adobe to finish. If the old database waits for its signal thread, a narrowly scoped helper sends SIGUSR2 only to the service's direct child matching the configured workspace's mysqld path. The database then performs its normal shutdown. The service has its own process group so unloading the controller does not prematurely terminate its database.

The bytecode agent bundles [ASM 3.3.1](https://repo.maven.apache.org/maven2/asm/asm-all/3.3.1/), with its BSD license in `src/versioncue/vendor/ASM-LICENSE.txt` and the built payload. Adobe jars and the exported JDK are not modified.

## Build and validation

On Snow Leopard with Xcode 3.2, the PowerPC compiler, and Java 6 javac:

```
bash src/versioncue/build.sh
```

Copy `bin/versioncue` to the Mojave checkout before installing. `src/versioncue/NullChecksTest.java` exercises twelve null operations, wide arguments/fields, array operations, and constructors. Compile it for Java 5 and run with the exported PPC Java using `-Xint -javaagent:/Library/Rosettus/versioncue/nullchecks.jar`.

Verified on Mojave:

- Original installed server reaches Running; web administration and login pages respond.
- Original preference pane shows On with enabled controls and no native Java installation prompt.
- Login-user status/start/stop commands use the service protocol.
- Repeated shutdown/start cycles complete; active LaunchDaemon removal drains the server with exit status 0 and database shutdown completes.
- LaunchDaemon reinstall starts the service again using existing AutoStart settings.
- Null-check regression completes 30,000 iterations with exactly 10,000 caught exceptions; the focused twelve-operation test passes.

No reboot or authenticated project check-in/check-out was tested. Interpreter mode affects performance. Treat important workspaces as experimental until those workflows have been validated with backups.

**"Unable to start database."** A database process left over from an earlier run (for example after a forced quit) still holds the workspace. Quit the leftover `mysqld` in Activity Monitor (or stop the service with `versioncue stop`) and start Version Cue again.
