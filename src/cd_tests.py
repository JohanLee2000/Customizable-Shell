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
sendline("mkdir cd-test-d")
sendline("cd cd-test-d")
sendline("pwd")

# sendline("cd")
# sendline("pwd")


expectedoutput = "cd-test-d"
# expectedoutput2 = "/home/ugrads/majors/kevin20"

expect_exact(expectedoutput, "expected glob expansion %s" % (expectedoutput))
# expect_exact(expectedoutput2, "expected glob expansion %s" % (expectedoutput2))
expect_prompt("not correct")

test_success()
