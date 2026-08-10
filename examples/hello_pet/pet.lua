-- A complete pet: it appears, it reacts to two buttons, it says how it feels.
kf.background(kf.color(232, 240, 216))

local body = kf.sprite("egg_idle_s")
body:move(96, 106)

local mood = kf.text("")
mood:move(4, 262)

kf.on_button("a", function() pet.feed() end)
kf.on_button("b", function() pet.play() end)

function on_frame(dt_ms)
    if pet.stage() == "egg" then
        body:sprite("egg_idle_s")
    else
        body:sprite("baby_neutral_s")
    end
    mood:set("HUNGER " .. math.floor(pet.hunger() / 1000) .. "%")
end
