STACK1_SIZE = 1024      # core 1 unused here: room below the RF dump-engine boundary
STACK0_SIZE = 7168      # bench app (buffers are static): room below the RF dump-engine boundary
XIP ?= all              # canaries + guards: code in flash keeps RAM below the RF dump-engine boundary
