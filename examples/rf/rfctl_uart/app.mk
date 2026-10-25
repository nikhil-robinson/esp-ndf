XIP ?= all              # canaries + guards: code in flash keeps RAM below the RF dump-engine boundary
STACK1_SIZE ?= 4096       # core 1 never started here: its stack only takes RAM below the RF dump-engine boundary
