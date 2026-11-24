function autoexec init()
{
    for (;;)
    {
        players = getplayers();
        for (i = 0; i < players.size; i++)
        {
            p = players[i];
            if (!isdefined(p) || p istestclient() || !p ishost() || isdefined(p.t7mp_class_watch)) continue;
            p.t7mp_class_watch = true;
            p thread watch_spawns();
        }
        wait 0.1;
    }
}
function watch_spawns()
{
    self endon("disconnect");
    if (isalive(self)) self thread apply_primary();
    for (;;)
    {
        self waittill("spawned_player");
        self notify("t7mp_new_loadout");
        self thread apply_primary();
    }
}
function apply_primary()
{
    self endon("disconnect");
    self endon("death");
    self endon("t7mp_new_loadout");
    for (tick = 0; tick < 40; tick++)
    {
        wait 0.05;
        classkey = "";
        // The T7 field named class uses a reserved word in ACTS.
        if (isdefined(self.var_f34c06a3)) classkey = self.var_f34c06a3;
        if (isdefined(self.currentclass)) classkey = self.currentclass;
        if (isdefined(self.pers["class"])) classkey = self.pers["class"];
        if (classkey == "" && isdefined(self.class_num)) classkey = self.class_num;
        if (isint(classkey)) classkey = "custom" + (classkey + 1);
        if (classkey == "") continue;
        id = getdvarstring("t7mp_primary_" + classkey);
        if (id != "t5dw_msmc_mp" && id != "t5dw_famas_mp") continue;
        base = "smg_standard_mp";
        if (id == "t5dw_famas_mp") base = "ar_standard_mp";
        current = self getcurrentweapon();
        if (current != getweapon(base)) continue;
        weapon = getweapon(id);
        if (!isdefined(weapon)) return;
        self giveweapon(weapon);
        if (!self hasweapon(weapon)) return;
        self switchtoweapon(weapon);
        self takeweapon(current);
        return;
    }
}
