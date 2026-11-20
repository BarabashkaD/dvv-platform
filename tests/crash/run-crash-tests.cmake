# The crash test (spec §8): runs dvv_platform_crash_child in each crash mode and checks the crash
# handler's stderr lines, its files, the report of an earlier crash and the cases where it is off.
# Called by the dvv_platform.crash test with -DCHILD (the child executable) and -DWORK_DIR (a
# scratch folder of the build tree). Each case runs in its own fresh folder <WORK_DIR>/<case> with
# the user data folder pointed into it (APPDATA, HOME; XDG_DATA_HOME unset), so no folder of the
# crash folder chain exists before the first crash; the child runs in the case folder, where a Linux
# core file lands. Every problem is collected and listed at the end; without one, WORK_DIR is
# removed.
cmake_minimum_required(VERSION 4.2)

foreach(required IN ITEMS CHILD WORK_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "pass -D${required}=<value>")
    endif()
endforeach()

set(APP "dvv-crash-test")
set(PROCESS "crash-child")

# CMake's regular expressions have no {n} repeat.
string(REPEAT "[0-9]" 8 digits_8)
string(REPEAT "[0-9]" 6 digits_6)
string(REPEAT "[0-9a-f]" 8 hex_8)
set(snapshot_name_regex "^${PROCESS}-${digits_8}-${digits_6}-[0-9]+\\.dvvlog$")
set(crash_log_regex "^${APP}: crash log (.+)$")
set(crash_dump_regex "^${APP}: crash dump (.+)$")
set(report_regex "crash child marker 1 \\[[^]\r\n]*\\] \\(from crash snapshot\\)")

# The user data root of the case folder (dvv::paths::userDataDir without the app name) and the
# folder chain the first crash creates, both relative to the case folder.
if(CMAKE_HOST_WIN32)
    set(data_root "AppData/Roaming")
    set(cause_regex "^${APP}: crash exception 0x${hex_8}$")
    set(abort_cause_regex "^${APP}: crash abort$")
elseif(CMAKE_HOST_APPLE)
    set(data_root "home/Library/Application Support")
    set(cause_regex "^${APP}: crash signal [0-9]+$")
else()
    set(data_root "home/.local/share")
    set(cause_regex "^${APP}: crash signal [0-9]+$")
endif()
set(crashes_folder "${data_root}/${APP}/crashes")
set(folder_chain "")
set(chain_prefix "")
string(REPLACE "/" ";" chain_parts "${crashes_folder}")
foreach(part IN LISTS chain_parts)
    if(chain_prefix STREQUAL "")
        set(chain_prefix "${part}")
    else()
        set(chain_prefix "${chain_prefix}/${part}")
    endif()
    list(APPEND folder_chain "${chain_prefix}")
endforeach()

# problem(<text>): records a problem with the last run's case, mode, result, stdout and stderr.
function(problem text)
    get_property(count GLOBAL PROPERTY crash_problem_count)
    if(NOT count)
        set(count 0)
    endif()
    math(EXPR count "${count} + 1")
    set_property(GLOBAL PROPERTY crash_problem_count ${count})
    set(mode "${run_mode}")
    if(mode STREQUAL "")
        set(mode "(none)")
    endif()
    string(CONCAT entry
        "[${count}] ${text}\n"
        "    case: ${run_case}\n"
        "    mode: ${mode}\n"
        "    result: ${run_result}\n"
        "    stdout:\n${run_stdout}\n"
        "    stderr:\n${run_stderr}\n")
    set_property(GLOBAL APPEND_STRING PROPERTY crash_problems "${entry}")
endfunction()

# fresh_case(<case>): an empty <WORK_DIR>/<case> with the user data folder inside it.
function(fresh_case case_name)
    set(case_dir "${WORK_DIR}/${case_name}")
    file(REMOVE_RECURSE "${case_dir}")
    file(MAKE_DIRECTORY "${case_dir}")
    set(ENV{APPDATA} "${case_dir}/AppData/Roaming")
    set(ENV{HOME} "${case_dir}/home")
    unset(ENV{XDG_DATA_HOME})
endfunction()

# run_child(<case> <mode or ""> [PROCESS <name>] [APP <name>]): runs the child in <WORK_DIR>/<case>
# and sets run_case, run_mode, run_result, run_stdout, run_stderr and run_seconds in the caller.
function(run_child case_name mode)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "PROCESS;APP" "")
    if(NOT DEFINED arg_PROCESS)
        set(arg_PROCESS "${PROCESS}")
    endif()
    set(command "${CHILD}" "${arg_PROCESS}")
    if(DEFINED arg_APP)
        list(APPEND command "${arg_APP}")
    endif()
    if(mode STREQUAL "")
        unset(ENV{DVV_PLATFORM_TEST_CRASH})
    else()
        set(ENV{DVV_PLATFORM_TEST_CRASH} "${mode}")
    endif()
    string(TIMESTAMP started "%s")
    execute_process(COMMAND ${command}
                    WORKING_DIRECTORY "${WORK_DIR}/${case_name}"
                    TIMEOUT 60
                    RESULT_VARIABLE result
                    OUTPUT_VARIABLE out
                    ERROR_VARIABLE err)
    string(TIMESTAMP ended "%s")
    unset(ENV{DVV_PLATFORM_TEST_CRASH})
    math(EXPR seconds "${ended} - ${started}")
    set(run_case "${case_name}" PARENT_SCOPE)
    set(run_mode "${mode}" PARENT_SCOPE)
    set(run_result "${result}" PARENT_SCOPE)
    set(run_stdout "${out}" PARENT_SCOPE)
    set(run_stderr "${err}" PARENT_SCOPE)
    set(run_seconds "${seconds}" PARENT_SCOPE)
endfunction()

# count_lines(<out> <text> <regex>): the number of lines of <text> that match <regex>; the first
# match's CMAKE_MATCH_1 goes to <out>_1.
function(count_lines out text regex)
    string(REPLACE "\r\n" "\n" text "${text}")
    set(count 0)
    set(first "")
    while(NOT text STREQUAL "")
        string(FIND "${text}" "\n" end)
        if(end EQUAL -1)
            set(line "${text}")
            set(text "")
        else()
            string(SUBSTRING "${text}" 0 ${end} line)
            math(EXPR next "${end} + 1")
            string(SUBSTRING "${text}" ${next} -1 text)
        endif()
        if(line MATCHES "${regex}")
            if(count EQUAL 0)
                set(first "${CMAKE_MATCH_1}")
            endif()
            math(EXPR count "${count} + 1")
        endif()
    endwhile()
    set(${out} ${count} PARENT_SCOPE)
    set(${out}_1 "${first}" PARENT_SCOPE)
endfunction()

# ended_as_crash(<out>): whether the last run ended as a crash (macOS, Linux: a death by one of the
# crash signals, which execute_process reports as its signal text; Windows: the exception code of
# the mode, as a text, as hex or as an unsigned or signed decimal, or exit code 3 for "abort" and
# "terminate"). Any other text, such as execute_process's "no such file or directory", is not a crash.
function(ended_as_crash out)
    set(crashed FALSE)
    if(run_result MATCHES "[Tt]imeout")
        set(crashed FALSE)
    elseif(CMAKE_HOST_WIN32 AND run_mode MATCHES "^(abort|terminate)$")
        if(run_result STREQUAL "3")
            set(crashed TRUE)
        endif()
    elseif(CMAKE_HOST_WIN32)
        if(run_mode STREQUAL "stack")
            # 0xC00000FD
            set(regex "[Ss]tack overflow|[Cc]00000[Ff][Dd]|3221225725|-1073741571")
        else()
            # 0xC0000005
            set(regex
                "[Aa]ccess violation|[Ss]egmentation fault|[Cc]0000005|3221225477|-1073741819")
        endif()
        if(run_result MATCHES "${regex}")
            set(crashed TRUE)
        endif()
    else()
        # CMake 4.3's texts for SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP and SIGABRT, and the
        # spellings other versions use.
        string(CONCAT regex
            "^(Segmentation fault|Bus error|Illegal instruction|Floating-point exception|"
            "SIGTRAP|Trace/breakpoint trap|Subprocess aborted|Child aborted|Abort trap)$")
        if(run_result MATCHES "${regex}")
            set(crashed TRUE)
        endif()
    endif()
    set(${out} ${crashed} PARENT_SCOPE)
endfunction()

# check_exit_0(): the last run exited normally with 0.
function(check_exit_0)
    if(NOT run_result STREQUAL "0")
        problem("the process did not exit with 0")
    endif()
endfunction()

# check_crash(<out stem>): the last run crashed with the handler's lines and files (case 1); the
# snapshot's stem (or "") goes to <out stem>.
function(check_crash out_stem)
    set(case_dir "${WORK_DIR}/${run_case}")
    set(stem "")
    ended_as_crash(crashed)
    if(NOT crashed)
        problem("the process did not end as a crash")
    endif()
    set(mode_cause_regex "${cause_regex}")
    if(CMAKE_HOST_WIN32 AND run_mode MATCHES "^(abort|terminate)$")
        set(mode_cause_regex "${abort_cause_regex}")
    endif()
    count_lines(causes "${run_stderr}" "${mode_cause_regex}")
    if(NOT causes EQUAL 1)
        problem("stderr has ${causes} cause lines, expected 1")
    endif()
    count_lines(logs "${run_stderr}" "${crash_log_regex}")
    if(NOT logs EQUAL 1)
        problem("stderr has ${logs} \"crash log\" lines, expected 1")
    endif()
    if(CMAKE_HOST_WIN32)
        count_lines(dumps "${run_stderr}" "${crash_dump_regex}")
        if(NOT dumps EQUAL 1)
            problem("stderr has ${dumps} \"crash dump\" lines, expected 1")
        else()
            string(FIND "${run_stderr}" "${APP}: crash log " log_at)
            string(FIND "${run_stderr}" "${APP}: crash dump " dump_at)
            if(dump_at LESS log_at)
                problem("the \"crash dump\" line comes before the \"crash log\" line")
            endif()
        endif()
    endif()
    foreach(folder IN LISTS folder_chain)
        if(NOT IS_DIRECTORY "${case_dir}/${folder}")
            problem("the folder ${folder} of the crash folder chain does not exist")
        endif()
    endforeach()
    file(GLOB snapshots LIST_DIRECTORIES false RELATIVE "${case_dir}/${crashes_folder}"
         "${case_dir}/${crashes_folder}/*.dvvlog")
    list(LENGTH snapshots snapshot_count)
    # The snapshot is written as <stem>.dvvlog.part and renamed once written.
    file(GLOB parts LIST_DIRECTORIES false "${case_dir}/${crashes_folder}/*.part")
    if(parts)
        problem("the crashes folder has .part files: ${parts}")
    endif()
    if(NOT snapshot_count EQUAL 1)
        problem("the crashes folder has ${snapshot_count} .dvvlog files, expected 1")
    else()
        list(GET snapshots 0 snapshot)
        if(NOT snapshot MATCHES "${snapshot_name_regex}")
            problem("the snapshot name ${snapshot} does not match the grammar")
        endif()
        # HEX: in text mode, CMake 4.3 returns some binary files' first 8 bytes with a newline added.
        file(READ "${case_dir}/${crashes_folder}/${snapshot}" magic LIMIT 8 HEX)
        if(NOT magic STREQUAL "4456564c4f475331")  # "DVVLOGS1"
            problem("the snapshot ${snapshot} does not start with DVVLOGS1")
        endif()
        if(logs EQUAL 1)
            # Windows prints the native path, whose separators may be '\' or '/'.
            file(TO_CMAKE_PATH "${logs_1}" logged_path)
            cmake_path(GET logged_path FILENAME logged)
            if(NOT logged STREQUAL snapshot OR NOT EXISTS "${logged_path}")
                problem("the \"crash log\" line names ${logs_1}, not the snapshot ${snapshot}")
            endif()
        endif()
        string(REGEX REPLACE "\\.dvvlog$" "" stem "${snapshot}")
        if(CMAKE_HOST_WIN32)
            set(dump "${case_dir}/${crashes_folder}/${stem}.dmp")
            if(NOT EXISTS "${dump}")
                problem("the minidump ${stem}.dmp does not exist")
            else()
                file(SIZE "${dump}" dump_size)
                if(dump_size EQUAL 0)
                    problem("the minidump ${stem}.dmp is empty")
                endif()
            endif()
        endif()
    endif()
    set(${out_stem} "${stem}" PARENT_SCOPE)
endfunction()

# check_no_crashes_folder(): no "crashes" folder anywhere under the last run's case folder.
function(check_no_crashes_folder)
    file(GLOB_RECURSE entries LIST_DIRECTORIES true "${WORK_DIR}/${run_case}/*")
    foreach(entry IN LISTS entries)
        if(IS_DIRECTORY "${entry}" AND entry MATCHES "/crashes$")
            problem("a crashes folder exists: ${entry}")
        endif()
    endforeach()
endfunction()

# check_off(): the last run exited with 0, logged one "crash handler is off:" line and created no
# crashes folder (case 6).
function(check_off)
    check_exit_0()
    count_lines(offs "${run_stdout}" "crash handler is off:")
    if(NOT offs EQUAL 1)
        problem("stdout has ${offs} \"crash handler is off:\" lines, expected 1")
    endif()
    check_no_crashes_folder()
endfunction()

# 1. Each mode crashes with its lines and files (Windows "abort" and "terminate": exit code 3,
# "crash abort" and a dump without an exception record; macOS, Linux: both end by SIGABRT).
set(modes 1 stack threads exit abort terminate)
foreach(mode IN LISTS modes)
    fresh_case("mode-${mode}")
    run_child("mode-${mode}" "${mode}")
    check_crash(stem)
endforeach()

# 2. A report run reports the earlier crash once and renames its snapshot.
fresh_case(report)
run_child(report 1)
check_crash(stem)
run_child(report "")
check_exit_0()
if(NOT run_stdout MATCHES "${report_regex}")
    problem("stdout has no reported \"crash child marker 1\" line")
endif()
if(NOT stem STREQUAL "" AND NOT EXISTS "${WORK_DIR}/report/${crashes_folder}/${stem}.reported.dvvlog")
    problem("${stem}.reported.dvvlog does not exist")
endif()
run_child(report "")
check_exit_0()
if(run_stdout MATCHES "\\(from crash snapshot\\)")
    problem("a second report run reported the snapshot again")
endif()

# 3. A run under another process name does not report the crash.
fresh_case(two-names)
run_child(two-names 1)
check_crash(stem)
run_child(two-names "" PROCESS crash-child-2)
check_exit_0()
if(run_stdout MATCHES "an earlier run")
    problem("a run as crash-child-2 reported the crash of crash-child")
endif()

# 4. A crash inside a sink, with the synchronous logger, ends soon with its files.
fresh_case(sink)
run_child(sink sink)
check_crash(stem)
if(NOT run_seconds LESS 30)
    problem("the crash inside a sink took ${run_seconds} s, expected less than 30")
endif()

# 5. Chaining (macOS, Linux): the child's own SIGSEGV handler, installed first, runs after the
# crash handler.
if(NOT CMAKE_HOST_WIN32)
    fresh_case(chain)
    run_child(chain chain)
    if(NOT run_result STREQUAL "42")
        problem("the chained handler's exit code 42 is missing")
    endif()
    if(NOT run_stdout MATCHES "previous handler ran")
        problem("stdout has no \"previous handler ran\"")
    endif()
    file(GLOB snapshots "${WORK_DIR}/chain/${crashes_folder}/*.dvvlog")
    if(NOT snapshots)
        problem("the chained crash left no .dvvlog")
    endif()
endif()

# 6. Off: each run logs one "crash handler is off:" line, exits with 0 and creates nothing.
fresh_case(off-long-home)
string(REPEAT "a" 1100 long_name)
set(ENV{APPDATA} "${WORK_DIR}/off-long-home/${long_name}")
set(ENV{HOME} "${WORK_DIR}/off-long-home/${long_name}")
run_child(off-long-home "")
check_off()
fresh_case(off-process)
run_child(off-process "" PROCESS a/b)
check_off()
fresh_case(off-app-64)
string(REPEAT "a" 64 app_64)
run_child(off-app-64 "" APP "${app_64}")
check_off()
fresh_case(off-app-slash)
run_child(off-app-slash "" APP a/b)
check_off()
fresh_case(off-app-dotdot)
run_child(off-app-dotdot "" APP ..)
check_off()

# 7. A folder that cannot be made: the crash still ends at once, with its cause line and no
# "crash log" line.
fresh_case(blocked)
file(TOUCH "${WORK_DIR}/blocked/blocker")
set(ENV{APPDATA} "${WORK_DIR}/blocked/blocker/home")
set(ENV{HOME} "${WORK_DIR}/blocked/blocker/home")
run_child(blocked 1)
ended_as_crash(crashed)
if(NOT crashed)
    problem("the process did not end as a crash")
endif()
if(NOT run_seconds LESS 60)
    problem("the crash took ${run_seconds} s, expected less than 60")
endif()
count_lines(causes "${run_stderr}" "${cause_regex}")
if(NOT causes EQUAL 1)
    problem("stderr has ${causes} cause lines, expected 1")
endif()
count_lines(logs "${run_stderr}" "${crash_log_regex}")
if(NOT logs EQUAL 0)
    problem("stderr has a \"crash log\" line although the folder cannot be made")
endif()

# 8. A run without a crash creates nothing.
fresh_case(first-run)
run_child(first-run "")
check_exit_0()
count_lines(offs "${run_stdout}" "crash handler is off:")
if(NOT offs EQUAL 0)
    problem("stdout has a \"crash handler is off:\" line")
endif()
if(EXISTS "${WORK_DIR}/first-run/${data_root}/${APP}")
    problem("a run without a crash created ${data_root}/${APP}")
endif()

get_property(problems GLOBAL PROPERTY crash_problems)
if(NOT "${problems}" STREQUAL "")
    message(FATAL_ERROR "crash test problems:\n${problems}")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
