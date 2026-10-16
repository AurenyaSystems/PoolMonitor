idf_component_register(
    SRCS
        "src/telemetry.cpp"
        "src/cli_class.cpp"
    INCLUDE_DIRS "inc"
    REQUIRES
        drivers
        console
)
