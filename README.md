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
