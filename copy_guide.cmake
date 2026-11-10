# Copy README.txt with Windows line endings (invoked from a POST_BUILD step).
file(READ "${IN}" text)
string(REPLACE "\r\n" "\n" text "${text}")
string(REPLACE "\n" "\r\n" text "${text}")
file(WRITE "${OUT}" "${text}")
