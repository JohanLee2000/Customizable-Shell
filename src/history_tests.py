#!/usr/bin/python
#
# Tests the functionality of gback's glob implement
# Also serves as example of how to write your own
# custom functionality tests.
#
import atexit, proc_check, time
from testutils import *

console = setup_tests()

# ensure that shell prints expected prompt
expect_prompt()

#################################################################
# 
# Boilerplate ends here, now write your specific test.
#
#################################################################
# Step 1. Create a temporary directory and put a few files in it
# 
#


# make sure it gets cleaned up if we exit
sendline("sleep 1")
sendline("history")


expectedoutput1 = "1 sleep 1"
expectedoutput2 = "2 history"

expect(expectedoutput1, "expected glob expansion %s" % (expectedoutput1))
expect(expectedoutput2, "expected glob expansion %s" % (expectedoutput2))

# expect("1 sleep 1")
# expect_prompt("not correct")

# expect("2 history")
expect_prompt("not correct")

test_success()
