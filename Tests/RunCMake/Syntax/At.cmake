set(right "wrong")
set(var "\${right}")
# Not expanded here with the new policy.
set(ref "@var@")

string(CONFIGURE "${ref}" output)
message("-->${output}<--")
