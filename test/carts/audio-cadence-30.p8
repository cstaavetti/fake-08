pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- Alternate sound commands on consecutive game updates.
function _init()
 poke(0x3200,24,14)
 poke(0x3241,1)
 n=0
end
function _update()
 n+=1
 if n%2==1 then sfx(0,3) else sfx(-1,3) end
end
function _draw()
 cls()
end
