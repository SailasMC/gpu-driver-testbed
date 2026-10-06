/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /opt/android-sdk/build-tools/35.0.0/aidl --lang=java -I /root/shizuku-api/aidl/src/main/aidl -p /opt/android-sdk/platforms/android-34/framework.aidl -o /root/appsrc/src /root/shizuku-api/aidl/src/main/aidl/moe/shizuku/server/IShizukuApplication.aidl
 */
package moe.shizuku.server;
public interface IShizukuApplication extends android.os.IInterface
{
  /** Default implementation for IShizukuApplication. */
  public static class Default implements moe.shizuku.server.IShizukuApplication
  {
    @Override public void bindApplication(android.os.Bundle data) throws android.os.RemoteException
    {
    }
    @Override public void dispatchRequestPermissionResult(int requestCode, android.os.Bundle data) throws android.os.RemoteException
    {
    }
    // Sui only
    @Override public void showPermissionConfirmation(int requestUid, int requestPid, java.lang.String requestPackageName, int requestCode) throws android.os.RemoteException
    {
    }
    @Override
    public android.os.IBinder asBinder() {
      return null;
    }
  }
  /** Local-side IPC implementation stub class. */
  public static abstract class Stub extends android.os.Binder implements moe.shizuku.server.IShizukuApplication
  {
    /** Construct the stub at attach it to the interface. */
    @SuppressWarnings("this-escape")
    public Stub()
    {
      this.attachInterface(this, DESCRIPTOR);
    }
    /**
     * Cast an IBinder object into an moe.shizuku.server.IShizukuApplication interface,
     * generating a proxy if needed.
     */
    public static moe.shizuku.server.IShizukuApplication asInterface(android.os.IBinder obj)
    {
      if ((obj==null)) {
        return null;
      }
      android.os.IInterface iin = obj.queryLocalInterface(DESCRIPTOR);
      if (((iin!=null)&&(iin instanceof moe.shizuku.server.IShizukuApplication))) {
        return ((moe.shizuku.server.IShizukuApplication)iin);
      }
      return new moe.shizuku.server.IShizukuApplication.Stub.Proxy(obj);
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
        case TRANSACTION_bindApplication:
        {
          android.os.Bundle _arg0;
          _arg0 = _Parcel.readTypedObject(data, android.os.Bundle.CREATOR);
          this.bindApplication(_arg0);
          break;
        }
        case TRANSACTION_dispatchRequestPermissionResult:
        {
          int _arg0;
          _arg0 = data.readInt();
          android.os.Bundle _arg1;
          _arg1 = _Parcel.readTypedObject(data, android.os.Bundle.CREATOR);
          this.dispatchRequestPermissionResult(_arg0, _arg1);
          break;
        }
        case TRANSACTION_showPermissionConfirmation:
        {
          int _arg0;
          _arg0 = data.readInt();
          int _arg1;
          _arg1 = data.readInt();
          java.lang.String _arg2;
          _arg2 = data.readString();
          int _arg3;
          _arg3 = data.readInt();
          this.showPermissionConfirmation(_arg0, _arg1, _arg2, _arg3);
          reply.writeNoException();
          break;
        }
        default:
        {
          return super.onTransact(code, data, reply, flags);
        }
      }
      return true;
    }
    private static class Proxy implements moe.shizuku.server.IShizukuApplication
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
      @Override public void bindApplication(android.os.Bundle data) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _Parcel.writeTypedObject(_data, data, 0);
          boolean _status = mRemote.transact(Stub.TRANSACTION_bindApplication, _data, null, android.os.IBinder.FLAG_ONEWAY);
        }
        finally {
          _data.recycle();
        }
      }
      @Override public void dispatchRequestPermissionResult(int requestCode, android.os.Bundle data) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeInt(requestCode);
          _Parcel.writeTypedObject(_data, data, 0);
          boolean _status = mRemote.transact(Stub.TRANSACTION_dispatchRequestPermissionResult, _data, null, android.os.IBinder.FLAG_ONEWAY);
        }
        finally {
          _data.recycle();
        }
      }
      // Sui only
      @Override public void showPermissionConfirmation(int requestUid, int requestPid, java.lang.String requestPackageName, int requestCode) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeInt(requestUid);
          _data.writeInt(requestPid);
          _data.writeString(requestPackageName);
          _data.writeInt(requestCode);
          boolean _status = mRemote.transact(Stub.TRANSACTION_showPermissionConfirmation, _data, _reply, 0);
          _reply.readException();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
      }
    }
    static final int TRANSACTION_bindApplication = (android.os.IBinder.FIRST_CALL_TRANSACTION + 1);
    static final int TRANSACTION_dispatchRequestPermissionResult = (android.os.IBinder.FIRST_CALL_TRANSACTION + 2);
    static final int TRANSACTION_showPermissionConfirmation = (android.os.IBinder.FIRST_CALL_TRANSACTION + 10000);
  }
  /** @hide */
  public static final java.lang.String DESCRIPTOR = "moe.shizuku.server.IShizukuApplication";
  public void bindApplication(android.os.Bundle data) throws android.os.RemoteException;
  public void dispatchRequestPermissionResult(int requestCode, android.os.Bundle data) throws android.os.RemoteException;
  // Sui only
  public void showPermissionConfirmation(int requestUid, int requestPid, java.lang.String requestPackageName, int requestCode) throws android.os.RemoteException;
  /** @hide */
  static class _Parcel {
    static private <T> T readTypedObject(
        android.os.Parcel parcel,
        android.os.Parcelable.Creator<T> c) {
      if (parcel.readInt() != 0) {
          return c.createFromParcel(parcel);
      } else {
          return null;
      }
    }
    static private <T extends android.os.Parcelable> void writeTypedObject(
        android.os.Parcel parcel, T value, int parcelableFlags) {
      if (value != null) {
        parcel.writeInt(1);
        value.writeToParcel(parcel, parcelableFlags);
      } else {
        parcel.writeInt(0);
      }
    }
  }
}
