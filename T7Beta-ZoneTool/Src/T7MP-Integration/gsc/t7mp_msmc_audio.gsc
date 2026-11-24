function autoexec init()
{
    setdvar("t7mp_msmc_audio", "");
    setdvar("t7mp_port_shot", "");
    level.msmc_audio_serial = 0;
    level thread find_local_host();
}
function find_local_host()
{
    for (;;)
    {
        players = getplayers();
        for (i = 0; i < players.size; i++)
        {
            p = players[i];
            if (!isdefined(p) || p istestclient() || !p ishost() || isdefined(p.msmc_audio_started)) continue;
            p.msmc_audio_started = true;
            p thread fire_audio();
            p thread reload_audio();
        }
        wait 0.25;
    }
}
function emit_audio(event)
{
    level.msmc_audio_serial++;
    if (event == "fire" || event == "famas_fire")
        setdvar("t7mp_port_shot", event + ":" + level.msmc_audio_serial);
    else
        setdvar("t7mp_msmc_audio", event + ":" + level.msmc_audio_serial);
}
function port_name()
{
    if (!isalive(self)) return "";
    current = self getcurrentweapon();
    if (current == getweapon("t5dw_msmc_mp")) return "msmc";
    if (current == getweapon("t5dw_famas_mp")) return "famas";
    return "";
}
function fire_audio()
{
    self endon("disconnect");
    for (;;)
    {
        self waittill("weapon_fired");
        name = self port_name();
        if (name == "msmc") emit_audio("fire");
        if (name == "famas") emit_audio("famas_fire");
    }
}
function reload_audio()
{
    self endon("disconnect");
    previous = "";
    wasReloading = false;
    lastAmmo = 0;
    for (;;)
    {
        name = self port_name();
        if (previous != name && wasReloading) emit_audio("stop");
        if (previous != name) wasReloading = false;
        previous = name;
        if (name != "")
        {
            reloading = self isreloading();
            if (reloading && !wasReloading)
            {
                event = "reload";
                if (lastAmmo == 0) event = "reload_empty";
                if (name == "famas") event = "famas_" + event;
                emit_audio(event);
            }
            if (!reloading && wasReloading) emit_audio("stop");
            wasReloading = reloading;
            lastAmmo = self getweaponammoclip(self getcurrentweapon());
        }
        else
        {
            wasReloading = false;
        }
        wait 0.02;
    }
}
