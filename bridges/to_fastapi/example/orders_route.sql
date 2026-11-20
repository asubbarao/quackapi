-- what: the bridge's example route: a customer's orders since a date, each with its lines, over two small
--   tables. NOT NULL columns, an ENUM, a DECIMAL, a TIMESTAMP and a list of structs exercise every path of
--   route_columns.sql and quack_to_fastapi.sql.
-- run on: the quackapi server, once. GET /customers/<customer id>/orders?since=2026-01-01
CREATE TYPE IF NOT EXISTS example_order_status AS ENUM ('placed', 'shipped', 'cancelled');

CREATE OR REPLACE TABLE example_orders (
    id UUID NOT NULL, customer_id UUID NOT NULL, status example_order_status NOT NULL,
    total DECIMAL(12,2), placed_at TIMESTAMP NOT NULL, note VARCHAR
);
CREATE OR REPLACE TABLE example_order_lines (
    order_id UUID NOT NULL, sku VARCHAR NOT NULL, quantity INTEGER NOT NULL, unit_price DECIMAL(12,2)
);
INSERT INTO example_orders VALUES
    ('00000000-0000-0000-0000-000000000001', '00000000-0000-0000-0000-00000000000a', 'placed', 30.00, '2026-03-01 10:00', NULL),
    ('00000000-0000-0000-0000-000000000002', '00000000-0000-0000-0000-00000000000a', 'shipped', NULL, '2026-04-01 09:30', 'gift'),
    ('00000000-0000-0000-0000-000000000003', '00000000-0000-0000-0000-00000000000b', 'cancelled', 12.50, '2026-05-01 17:45', NULL);
INSERT INTO example_order_lines VALUES
    ('00000000-0000-0000-0000-000000000001', 'A-1', 2, 10.00), ('00000000-0000-0000-0000-000000000001', 'B-2', 1, 10.00),
    ('00000000-0000-0000-0000-000000000002', 'C-3', 4, NULL), ('00000000-0000-0000-0000-000000000003', 'A-1', 1, 12.50);

CREATE OR REPLACE ROUTE example_orders GET '/customers/:customer_id/orders' AS
WITH order_lines AS (
    SELECT l.order_id, list({'sku': l.sku, 'quantity': l.quantity, 'unit_price': l.unit_price} ORDER BY l.sku) AS lines
    FROM main.example_order_lines AS l
    GROUP BY l.order_id
)
SELECT o.id, o.status, o.total, o.placed_at, o.note, coalesce(ol.lines, []) AS lines
FROM main.example_orders AS o
LEFT JOIN order_lines AS ol ON ol.order_id = o.id
WHERE o.customer_id = $customer_id::UUID AND o.placed_at >= $since::DATE
ORDER BY o.placed_at;
