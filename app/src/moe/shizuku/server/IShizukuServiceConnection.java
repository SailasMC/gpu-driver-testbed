/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /opt/android-sdk/build-tools/35.0.0/aidl --lang=java -I /root/shizuku-api/aidl/src/main/aidl -p /opt/android-sdk/platforms/android-34/framework.aidl -o /root/appsrc/src /root/shizuku-api/aidl/src/main/aidl/moe/shizuku/server/IShizukuServiceConnection.aidl
 */
package moe.shizuku.server;
public interface IShizukuServiceConnection extends android.os.IInterface
{
  /** Default implementation for IShizukuServiceConnection. */
  public static class Default implements moe.shizuku.server.IShizukuServiceConnection
  {
    @Override public void connected(android.os.IBinder service) throws android.os.RemoteException
    {
    }
    @Override public void died() throws android.os.RemoteException
    {
    }
    @Override
    public android.os.IBinder asBinder() {
      return null;
    }
  }
  /** Local-side IPC implementation stub class. */
  public static abstract class Stub extends android.os.Binder implements moe.shizuku.server.IShizukuServiceConnection
  {
    /** Construct the stub at attach it to the interface. */
    @SuppressWarnings("this-escape")
    public Stub()
    {
      this.attachInterface(this, DESCRIPTOR);
    }
    /**
     * Cast an IBinder object into an moe.shizuku.server.IShizukuServiceConnection interface,
     * generating a proxy if needed.
     */
    public static moe.shizuku.server.IShizukuServiceConnection asInterface(android.os.IBinder obj)
    {
      if ((obj==null)) {
        return null;
      }
      android.os.IInterface iin = obj.queryLocalInterface(DESCRIPTOR);
      if (((iin!=null)&&(iin instanceof moe.shizuku.server.IShizukuServiceConnection))) {
        return ((moe.shizuku.server.IShizukuServiceConnection)iin);
      }
      return new moe.shizuku.server.IShizukuServiceConnection.Stub.Proxy(obj);
    }
    @Override public android.os.IBinder asBinder()
    {
      return this;
    }
    @Override public boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags) throws android.os.RemoteException
    {
      java.lang.String descriptor = DESCRIPTOR;
      if (code >= android.os.IBinder.FIRST_CALL_TRANSACTION && code <= android.os.IBinder.LAST_CALL_TRANSACTION) {
        data.enforceInterface(descriptor);
      }
      if (code == INTERFACE_TRANSACTION) {
        reply.writeString(descriptor);
        return true;
      }
      switch (code)
      {
        case TRANSACTION_connected:
        {
          android.os.IBinder _arg0;
          _arg0 = data.readStrongBinder();
          this.connected(_arg0);
          break;
        }
        case TRANSACTION_died:
        {
          this.died();
          break;
        }
        default:
        {
          return super.onTransact(code, data, reply, flags);
        }
      }
      return true;
    }
    private static class Proxy implements moe.shizuku.server.IShizukuServiceConnection
    {
      private android.os.IBinder mRemote;
      Proxy(android.os.IBinder remote)
      {
        mRemote = remote;
      }
      @Override public android.os.IBinder asBinder()
      {
        return mRemote;
      }
      public java.lang.String getInterfaceDescriptor()
      {
        return DESCRIPTOR;
      }
      @Override public void connected(android.os.IBinder service) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeStrongBinder(service);
          boolean _status = mRemote.transact(Stub.TRANSACTION_connected, _data, null, android.os.IBinder.FLAG_ONEWAY);
        }
        finally {
          _data.recycle();
        }
      }
      @Override public void died() throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          boolean _status = mRemote.transact(Stub.TRANSACTION_died, _data, null, android.os.IBinder.FLAG_ONEWAY);
        }
        finally {
          _data.recycle();
        }
      }
    }
    static final int TRANSACTION_connected = (android.os.IBinder.FIRST_CALL_TRANSACTION + 0);
    static final int TRANSACTION_died = (android.os.IBinder.FIRST_CALL_TRANSACTION + 1);
  }
  /** @hide */
  public static final java.lang.String DESCRIPTOR = "moe.shizuku.server.IShizukuServiceConnection";
  public void connected(android.os.IBinder service) throws android.os.RemoteException;
  public void died() throws android.os.RemoteException;
}
