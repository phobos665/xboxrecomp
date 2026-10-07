# guest_faults -- trapped guest pages through the real POSIX fault path
# (recomp_fault_install's signal handler, the fault_emulate.h route pieces, the
# guest arena and the host's decoder): device ranges, write-only trap pages,
# the 12 KB of collateral around a 4 KB trap on a 16 KB-page host, and a real
# null access still reaching the crash report. POSIX only; links only the
# platform library; no game files.

add_executable(guest_faults_test test_main.c)
target_link_libraries(guest_faults_test PRIVATE platform)

add_test(NAME guest_faults COMMAND guest_faults_test)
