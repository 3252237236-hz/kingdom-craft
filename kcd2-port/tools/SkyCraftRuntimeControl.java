import com.sun.tools.attach.VirtualMachine;
import java.lang.instrument.Instrumentation;
import java.nio.file.Path;

/** Stops only this port's hidden client through Minecraft's normal save/quit path. */
public final class SkyCraftRuntimeControl {
    public static void main(String[] args) throws Exception {
        if (args.length != 3) throw new IllegalArgumentException("PID agent.jar launch.cfg");
        VirtualMachine vm = VirtualMachine.attach(args[0]);
        try {
            var properties = vm.getSystemProperties();
            Path expected = Path.of(args[2]).toAbsolutePath().normalize();
            String config = properties.getProperty("fabric.dli.config", "");
            // Dev Launch Injector removes its config property after startup.
            // The configured run folder plus both port-only flags identify it.
            boolean configMatches = !config.isEmpty()
                && Path.of(config).toAbsolutePath().normalize().equals(expected);
            boolean runMatches = Path.of(properties.getProperty("user.dir", ""))
                .toAbsolutePath().normalize().equals(expected.getParent().getParent()
                    .getParent().resolve("run"))
                && "true".equals(properties.getProperty("skycraft.kcd2.passthrough"))
                && "true".equals(properties.getProperty("skycraft.startHidden"));
            if (!configMatches && !runMatches)
                throw new IllegalStateException("Not the configured SkyCraft client");
            vm.loadAgent(Path.of(args[1]).toAbsolutePath().toString(), "stop");
        } finally {
            vm.detach();
        }
    }

    public static void agentmain(String command, Instrumentation instrumentation) throws Exception {
        if (!"stop".equals(command)) throw new IllegalArgumentException("Unknown command");
        boolean gameRunning = ProcessHandle.allProcesses().anyMatch(process ->
            process.info().command().map(path ->
                Path.of(path).getFileName().toString().equalsIgnoreCase("KingdomCome.exe"))
                .orElse(false));
        if (gameRunning) throw new IllegalStateException("KCD2 is running; client retained");
        for (Class<?> type : instrumentation.getAllLoadedClasses()) {
            if (!type.getName().equals("net.minecraft.client.Minecraft")) continue;
            Object client = type.getMethod("getInstance").invoke(null);
            var stop = type.getMethod("stop");
            type.getMethod("execute", Runnable.class).invoke(client, (Runnable) () -> {
                try { stop.invoke(client); }
                catch (ReflectiveOperationException failure) { throw new IllegalStateException(failure); }
            });
            System.out.println("SkyCraft runtime: requested normal Minecraft save and shutdown");
            return;
        }
        throw new IllegalStateException("Minecraft client class is not loaded");
    }
}
