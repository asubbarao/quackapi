-- Hand-written, not generated: what the route reads. Here the example's two tables in memory; in a real
-- project, the ATTACH of the database the quackapi route reads, under the same catalog name.
CREATE TYPE example_order_status AS ENUM ('placed', 'shipped', 'cancelled');
CREATE TABLE example_orders (
    id UUID NOT NULL, customer_id UUID NOT NULL, status example_order_status NOT NULL,
    total DECIMAL(12,2), placed_at TIMESTAMP NOT NULL, note VARCHAR
);
CREATE TABLE example_order_lines (
    order_id UUID NOT NULL, sku VARCHAR NOT NULL, quantity INTEGER NOT NULL, unit_price DECIMAL(12,2)
);
INSERT INTO example_orders VALUES
    ('00000000-0000-0000-0000-000000000001', '00000000-0000-0000-0000-00000000000a', 'placed', 30.00, '2026-03-01 10:00', NULL),
    ('00000000-0000-0000-0000-000000000002', '00000000-0000-0000-0000-00000000000a', 'shipped', NULL, '2026-04-01 09:30', 'gift'),
    ('00000000-0000-0000-0000-000000000003', '00000000-0000-0000-0000-00000000000b', 'cancelled', 12.50, '2026-05-01 17:45', NULL);
INSERT INTO example_order_lines VALUES
    ('00000000-0000-0000-0000-000000000001', 'A-1', 2, 10.00), ('00000000-0000-0000-0000-000000000001', 'B-2', 1, 10.00),
    ('00000000-0000-0000-0000-000000000002', 'C-3', 4, NULL), ('00000000-0000-0000-0000-000000000003', 'A-1', 1, 12.50);
