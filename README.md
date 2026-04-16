## 准备

建议所有ubus/libubox等全部为Debug编译

设置ubus socket为普通用户可访问

```
SET(UBUS_UNIX_SOCKET "/tmp/ubus.sock")
```

创建acl文件允许普通用户添加/查看对象:

`/usr/share/acl.d/hrimfaxi.json `:
```
{
    "user": "hrimfaxi",
    "access": {
        "tumgrd": {
            "methods": [ "*" ]
        }
    },
    "publish": [ "tumgrd" ]
}
```


## 调用

注册
```
```
ubus -s /tmp/ubus.sock call tumgrd register '{"uid": "my-node-01", "server_host": "192.168.1.100", "client_port": 1443, "server_port": 14801, "psk": "scinfaxi" }'

状态
```
ubus -s /tmp/ubus.sock call tumgrd status
```

刷新
```
ubus -s /tmp/ubus.sock call tumgrd refresh '{"all":true, "force": true}'
ubus -s /tmp/ubus.sock call tumgrd refresh '{"uid":"x1", "server_host":"a.com", "client_port":1443, "force":true}'
```

反注册:
```
ubus -s /tmp/ubus.sock call tumgrd deregister '{"uid":"x1", "server_host":"a.com", "client_port":1443}'
```
