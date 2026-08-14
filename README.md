# XGC2 ROS2 Tools Adapter

This target-side Adapter Runtime provider exposes two bounded typed operations:

- `xgc.ros2.topic.publish@1/publish`
- `xgc.ros2.service.call@1/call`

It uses `rclcpp` generic publishers/clients plus installed ROS introspection type
support. It does not invoke `ros2`, a shell, SSH, or terminal commands. One
on-demand process is shared per frozen ROS domain/RMW context and exits after
the Adapter Runtime idle timeout.

Workflow inputs describe only the topic/service operation. `ROS_DOMAIN_ID` and
`RMW_IMPLEMENTATION` are resolved by the target Agent before invocation and are
part of the Adapter instance identity. Calls that may have reached ROS but lose
their response terminate as `uncertain` and are never automatically retried.

## Build and package

The supported build target is Ubuntu 24.04 with ROS 2 Jazzy. The repository
ships one release path for both supported architectures:

```bash
.xgc2/scripts/build_debs_in_docker.sh \
  --work-dir /tmp/xgc2-ros2-tools-adapter-build \
  --output-dir "$PWD/debs"
```

The builder compiles the real C++ provider, runs its C++ and manifest tests,
installs into an isolated root, creates the Debian package, installs that Deb,
and validates ROS package discovery, dynamic-library resolution, runtime
manifests, and package ownership. CI and the release train invoke the same
builder on native `amd64` and `arm64` runners.

APT publication is owned only by the central `xgc2-devops` release train. This
repository produces a trusted Deb and build manifest; it contains no APT
credentials or publishing implementation.
