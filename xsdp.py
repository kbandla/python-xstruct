# xsdp.py - a Python script that demonstrates the capabilities of the
# xstruct.structdef function (xsdp = XStruct Demonstration Protocol)
# written by Robin Boerdijk (boerdijk@my-deja.com)
# last modified: 1999-10-06

import xstruct
import sys

print """>>> XsdpMessage = xstruct.structdef(xstruct.big_endian, [
  ("magic",        (xstruct.string, 4),   "XSDP",   xstruct.readonly),
  ("version",      (xstruct.octet,  2),   (1, 0)), 
  ("byte_order",   (xstruct.octet,  1),    0,       xstruct.readonly), 
  ("message_type", (xstruct.octet,  1)), 
  ("correl_id",    (xstruct.unsigned_long, 1)),
  ("data",         (xstruct.string, 16))
])""" 

XsdpMessage = xstruct.structdef(xstruct.big_endian, [
  ("magic",        (xstruct.string, 4),   "XSDP",   xstruct.readonly),
  ("version",      (xstruct.octet,  2),   (1, 0)), 
  ("byte_order",   (xstruct.octet,  1),    0,       xstruct.readonly), 
  ("message_type", (xstruct.octet,  1)), 
  ("correl_id",    (xstruct.unsigned_long, 1)),
  ("data",         (xstruct.string, 16))
]) 

print

print ">>> XsdpMessage"
print XsdpMessage

print

print ">>> msg = XsdpMessage()"

msg = XsdpMessage()

print ">>> print msg"
print msg

print

print ">>> msg.correl_id = 0x01020304"
msg.correl_id = 0x01020304

print ">>> msg.correl_id"
print msg.correl_id

print

print ">>> msg['data'] = \"Hello, World !\""
msg['data'] = "Hello, World !"

print ">>> msg['data']"
print msg['data']

print

print ">>> msg.magic = \"XXXX\""
try:
    msg.magic = "XXXX"
except:
    print "Traceback (innermost last):"
    print "  File \"<stdin>\", line 1, in ?"
    print "%s: %s" % (sys.exc_type, sys.exc_value)

print

print ">>> buf = str(msg)"
buf = str(msg)

print ">>> buf"
print repr(buf)

print

print ">>> open(\"tmp\", \"w\").write(msg)"
open("tmp", "w").write(msg)

print ">>> open(\"tmp\", \"r\").read()"
print repr(open("tmp", "r").read())

print

print ">>> msg2 = XsdpMessage(buf)"
msg2 = XsdpMessage(buf)

print ">>> msg2"
print msg2

print

print ">>> msg3 = XsdpMessage()"
msg3 = XsdpMessage()

print ">>> open(\"tmp\", \"r\").readinto(msg3)"
open("tmp", "r").readinto(msg3)

print ">>> msg3"
print msg3

