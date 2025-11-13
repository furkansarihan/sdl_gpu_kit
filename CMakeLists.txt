cmake_minimum_required(VERSION 3.15)
project(app)

# find libraries
find_package(SDL3 REQUIRED)
find_package(glm REQUIRED)
find_package(TinyGLTF REQUIRED)
# find_package(... REQUIRED)

# Add all .cpp files in the src directory and its subdirectories
file(GLOB_RECURSE CPP_SOURCES src/*.cpp)

# Add all .h files in the src directory and its subdirectories
file(GLOB_RECURSE HEADER_FILES src/*.h)

# Create the engine library target
add_executable(${PROJECT_NAME} src/main.cpp ${CPP_SOURCES} ${HEADER_FILES})

# Include headers
target_include_directories(${PROJECT_NAME} PRIVATE ${ENIGINE_DIR}/src)

# Link libraries
target_link_libraries(${PROJECT_NAME} SDL3::SDL3)
target_link_libraries(${PROJECT_NAME} glm::glm)
target_link_libraries(${PROJECT_NAME} TinyGLTF::TinyGLTF)
# target_link_libraries(${PROJECT_NAME} ...)

# Copy assets
file(COPY ${CMAKE_SOURCE_DIR}/assets DESTINATION ${CMAKE_BINARY_DIR}/)

# Collect all shader files from src directory
file(GLOB_RECURSE SHADER_SOURCES 
    "${CMAKE_SOURCE_DIR}/src/*.vert"
    "${CMAKE_SOURCE_DIR}/src/*.frag"
    "${CMAKE_SOURCE_DIR}/src/*.comp"
)

function(compile_shader SOURCE_FILE)
    # Get relative path from src directory
    file(RELATIVE_PATH REL_PATH "${CMAKE_SOURCE_DIR}/src" "${SOURCE_FILE}")
    
    # Create output path maintaining directory structure
    set(OUTPUT ${CMAKE_BINARY_DIR}/src/${REL_PATH})
    get_filename_component(OUTPUT_DIR ${OUTPUT} DIRECTORY)
    
    # Ensure output directory exists
    file(MAKE_DIRECTORY ${OUTPUT_DIR})
    
    if(APPLE)
        # GLSL -> SPIR-V
        set(SPV_OUTPUT ${OUTPUT}.spv)
        add_custom_command(
            OUTPUT ${SPV_OUTPUT}
            COMMAND glslc ${SOURCE_FILE} -o ${SPV_OUTPUT} -I ${CMAKE_SOURCE_DIR}/src
            DEPENDS ${SOURCE_FILE}
        )
        
        # SPIR-V -> Metal
        set(METAL_OUTPUT ${OUTPUT}.metal)
        add_custom_command(
            OUTPUT ${METAL_OUTPUT}
            COMMAND spirv-cross ${SPV_OUTPUT} --msl --output ${METAL_OUTPUT}
            DEPENDS ${SPV_OUTPUT}
        )
        
        # Metal -> AIR
        set(AIR_OUTPUT ${OUTPUT}.air)
        add_custom_command(
            OUTPUT ${AIR_OUTPUT}
            COMMAND xcrun -sdk macosx metal -c ${METAL_OUTPUT} -o ${AIR_OUTPUT}
            DEPENDS ${METAL_OUTPUT}
        )
        
        # AIR -> metallib
        set(METALLIB_OUTPUT ${OUTPUT}.metallib)
        add_custom_command(
            OUTPUT ${METALLIB_OUTPUT}
            COMMAND xcrun -sdk macosx metallib ${AIR_OUTPUT} -o ${METALLIB_OUTPUT}
            DEPENDS ${AIR_OUTPUT}
        )
        
        set(FINAL_OUTPUT ${METALLIB_OUTPUT})
    else()
        # Non-Apple platforms: just compile to SPIR-V
        add_custom_command(
            OUTPUT ${OUTPUT}
            COMMAND glslc ${SOURCE_FILE} -o ${OUTPUT} -I ${CMAKE_SOURCE_DIR}/src
            DEPENDS ${SOURCE_FILE}
        )
        set(FINAL_OUTPUT ${OUTPUT})
    endif()
    
    # Create unique target name from relative path
    string(REPLACE "/" "_" TARGET_NAME ${REL_PATH})
    string(REPLACE "." "_" TARGET_NAME ${TARGET_NAME})
    
    add_custom_target(shader_${TARGET_NAME} DEPENDS ${FINAL_OUTPUT})
    add_dependencies(${PROJECT_NAME} shader_${TARGET_NAME})
endfunction()

# Compile all discovered shaders
foreach(SHADER_FILE ${SHADER_SOURCES})
    compile_shader(${SHADER_FILE})
endforeach()
