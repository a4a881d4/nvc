package p is

    type int_ptr is access integer;     -- OK

    type bad1 is access foo;            -- Error

    type rec;

    type rec_ptr is access rec;

    type rec is record
        value : integer;
        link  : rec_ptr;
    end record;

    type int_vec is array (integer range <>) of integer;

    type int_vec_ptr is access int_vec;

    type string_ptr is access string;

end package;

package body p is

    procedure test is
        variable v : int_ptr;
        variable i : integer;
        variable r : rec_ptr;
        variable a : int_vec_ptr;
        variable s : string_ptr;
    begin
        v := null;                      -- OK
        i := null;                      -- Error
        deallocate(v);                  -- OK
        v := new integer;               -- OK
        v := new integer'(5);           -- OK
        v := new 5;                     -- Error
        v := new i;                     -- Error
        v.all := 5;                     -- OK
        v := 5;                         -- Error
        i := v.all + 5;                 -- OK
        r := new rec;                   -- OK
        r.all.value := 1;               -- OK
        r.value := 1;                   -- OK
        r.link := r;                    -- OK
        r.link := r.all;                -- Error
        i := r.value;                   -- OK
        r := r.all.link;                -- OK
        a := new int_vec(1 to 3);       -- OK
        a.all(5) := 2;                  -- OK
        a(5) := 2;                      -- OK
        a(1 to 2) := (1, 2);            -- OK
        s := new string'("");           -- OK
        s := new integer'(1);           -- Error
        s := new s(1 to 3);             -- Error
    end procedure;

end package body;
