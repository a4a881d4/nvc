entity one is
end entity;

entity two is
end two;

entity three is
end entity three;

entity four is
    port (
        a      : in integer := 4;
        b, bee : out bit;
        c      : inout integer;
        d      : buffer bit );
end entity;

entity five is
    generic (
        X : boolean;
        Y : integer := 2 * 5);
    port (
        signal p : out bit );
end entity;

entity six is
    attribute a of six : entity is 1;
end entity;
