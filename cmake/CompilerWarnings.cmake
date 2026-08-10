# Shared compiler warning flags for qualcomm-log-collector

function(set_compiler_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /WX-         # Don't treat warnings as errors (for now)
            /wd4100      # Unreferenced formal parameter
            /wd4189      # Local variable initialized but not referenced
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-unused-parameter
            -Wno-unused-variable
        )
    endif()
endfunction()
