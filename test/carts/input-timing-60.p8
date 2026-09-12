pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
function _init()
 frame=0
 was=false
 edges=0
 events=0
 misses=0
end
function _update60()
 local held=btn(0)
 local pressed=btnp(0)
 if held and not was then
  edges+=1
  if not pressed then misses+=1 end
 end
 if pressed then
  poke2(0x4400+events*2,frame)
  events+=1
 end
 poke2(0x4300,edges)
 poke2(0x4302,events)
 poke2(0x4304,misses)
 was=held
 frame+=1
end
function _draw()
 cls()
end
