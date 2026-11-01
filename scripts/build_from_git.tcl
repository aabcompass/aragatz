# Aragatz: clean project reconstruction from Git.
# Vivado 2023.1, XC7Z035.
# Does not modify the existing Vivado project.

set script_dir [file dirname [file normalize [info script]]]
set root [file normalize [file join $script_dir ..]]
set build_dir [file join $root build aragatz_git_check]

# Safety: never overwrite an existing project.
if {[llength [get_projects -quiet]] != 0} {
    error "Close the current Vivado project first"
}

if {[file exists $build_dir]} {
    error "Build directory already exists: $build_dir"
}

# Collect source files directly from the Git submodules.
set rtl_sources {}

foreach ip {camera_capture camera_test_gen} {
    set rtl_dir [file join $root ip_repo $ip rtl]

    if {![file isdirectory $rtl_dir]} {
        error "Missing submodule directory: $rtl_dir"
    }

    foreach f [glob -nocomplain -directory $rtl_dir *.sv *.v] {
        lappend rtl_sources $f
    }
}

# Module Reference adapters are mandatory.
foreach {ip adapter} {
    camera_capture camera_capture_bd.v
    camera_test_gen camera_test_gen_bd.v
} {
    set f [file join $root ip_repo $ip rtl $adapter]

    if {![file isfile $f]} {
        error "Missing Module Reference adapter: $f"
    }
}

# Create an independent project.
file mkdir [file dirname $build_dir]

create_project aragatz $build_dir \
    -part xc7z035ffg676-2

# Reference sources in place. Do NOT import/copy them.
add_files -norecurse -fileset sources_1 $rtl_sources

# Optional project-level constraints.
# Do not import the standalone-IP clock.xdc files.
set xdc_dir [file join $root constraints]

if {[file isdirectory $xdc_dir]} {
    foreach f [glob -nocomplain -directory $xdc_dir *.xdc] {
        add_files -norecurse -fileset constrs_1 $f
    }
}

update_compile_order -fileset sources_1

# Reconstruct the existing Block Design.
source [file join $script_dir create_bd.tcl]

set bd [get_files -quiet design_1.bd]

if {[llength $bd] != 1} {
    error "Block Design design_1.bd was not created"
}

open_bd_design $bd

validate_bd_design
save_bd_design

# Generate top-level HDL wrapper.
set wrapper [make_wrapper -files $bd -top]

add_files -norecurse -fileset sources_1 $wrapper

set_property top design_1_wrapper \
    [get_filesets sources_1]

update_compile_order -fileset sources_1

puts "========================================"
puts "Aragatz reconstructed from Git"
puts "Project: $build_dir"
puts "RTL files: [llength $rtl_sources]"
puts "Block Design: design_1"
puts "========================================"
