/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /opt/android-sdk/build-tools/35.0.0/aidl --lang=java -I /root/shizuku-api/aidl/src/main/aidl -p /opt/android-sdk/platforms/android-34/framework.aidl -o /root/appsrc/src /root/shizuku-api/aidl/src/main/aidl/moe/shizuku/server/IRemoteProcess.aidl
 */
package moe.shizuku.server;
public interface IRemoteProcess extends android.os.IInterface
{
  /** Default implementation for IRemoteProcess. */
  public static class Default implements moe.shizuku.server.IRemoteProcess
  {
    @Override public android.os.ParcelFileDescriptor getOutputStream() throws android.os.RemoteException
    {
      return null;
    }
    @Override public android.os.ParcelFileDescriptor getInputStream() throws android.os.RemoteException
    {
      return null;
    }
    @Override public android.os.ParcelFileDescriptor getErrorStream() throws android.os.RemoteException
    {
      return null;
    }
    @Override public int waitFor() throws android.os.RemoteException
    {
      return 0;
    }
    @Override public int exitValue() throws android.os.RemoteException
    {
      return 0;
    }
    @Override public void destroy() throws android.os.RemoteException
    {
    }
    @Override public boolean alive() throws android.os.RemoteException
    {
      return false;
    }
    @Override public boolean waitForTimeout(long timeout, java.lang.String unit) throws android.os.RemoteException
    {
      return false;
    }
    @Override
    public android.os.IBinder asBinder() {
      return null;
    }
  }
  /** Local-side IPC implementation stub class. */
  public static abstract class Stub extends android.os.Binder implements moe.shizuku.server.IRemoteProcess
  {
    /** Construct the stub at attach it to the interface. */
    @SuppressWarnings("this-escape")
    public Stub()
    {
      this.attachInterface(this, DESCRIPTOR);
    }
    /**
     * Cast an IBinder object into an moe.shizuku.server.IRemoteProcess interface,
     * generating a proxy if needed.
     */
    public static moe.shizuku.server.IRemoteProcess asInterface(android.os.IBinder obj)
    {
      if ((obj==null)) {
        return null;
      }
      android.os.IInterface iin = obj.queryLocalInterface(DESCRIPTOR);
      if (((iin!=null)&&(iin instanceof moe.shizuku.server.IRemoteProcess))) {
        return ((moe.shizuku.server.IRemoteProcess)iin);
      }
      return new moe.shizuku.server.IRemoteProcess.Stub.Proxy(obj);
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
        case TRANSACTION_getOutputStream:
        {
          android.os.ParcelFileDescriptor _result = this.getOutputStream();
          reply.writeNoException();
          _Parcel.writeTypedObject(reply, _result, android.os.Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
          break;
        }
        case TRANSACTION_getInputStream:
        {
          android.os.ParcelFileDescriptor _result = this.getInputStream();
          reply.writeNoException();
          _Parcel.writeTypedObject(reply, _result, android.os.Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
          break;
        }
        case TRANSACTION_getErrorStream:
        {
          android.os.ParcelFileDescriptor _result = this.getErrorStream();
          reply.writeNoException();
          _Parcel.writeTypedObject(reply, _result, android.os.Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
          break;
        }
        case TRANSACTION_waitFor:
        {
          int _result = this.waitFor();
          reply.writeNoException();
          reply.writeInt(_result);
          break;
        }
        case TRANSACTION_exitValue:
        {
          int _result = this.exitValue();
          reply.writeNoException();
          reply.writeInt(_result);
          break;
        }
        case TRANSACTION_destroy:
        {
          this.destroy();
          reply.writeNoException();
          break;
        }
        case TRANSACTION_alive:
        {
          boolean _result = this.alive();
          reply.writeNoException();
          reply.writeInt(((_result)?(1):(0)));
          break;
        }
        case TRANSACTION_waitForTimeout:
        {
          long _arg0;
          _arg0 = data.readLong();
          java.lang.String _arg1;
          _arg1 = data.readString();
          boolean _result = this.waitForTimeout(_arg0, _arg1);
          reply.writeNoException();
          reply.writeInt(((_result)?(1):(0)));
          break;
        }
        default:
        {
          return super.onTransact(code, data, reply, flags);
        }
      }
      return true;
    }
    private static class Proxy implements moe.shizuku.server.IRemoteProcess
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
      @Override public android.os.ParcelFileDescriptor getOutputStream() throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        android.os.ParcelFileDescriptor _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          boolean _status = mRemote.transact(Stub.TRANSACTION_getOutputStream, _data, _reply, 0);
          _reply.readException();
          _result = _Parcel.readTypedObject(_reply, android.os.ParcelFileDescriptor.CREATOR);
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public android.os.ParcelFileDescriptor getInputStream() throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        android.os.ParcelFileDescriptor _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          boolean _status = mRemote.transact(Stub.TRANSACTION_getInputStream, _data, _reply, 0);
          _reply.readException();
          _result = _Parcel.readTypedObject(_reply, android.os.ParcelFileDescriptor.CREATOR);
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public android.os.ParcelFileDescriptor getErrorStream() throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        android.os.ParcelFileDescriptor _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          boolean _status = mRemote.transact(Stub.TRANSACTION_getErrorStream, _data, _reply, 0);
          _reply.readException();
          _result = _Parcel.readTypedObject(_reply, android.os.ParcelFileDescriptor.CREATOR);
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public int waitFor() throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        int _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          boolean _status = mRemote.transact(Stub.TRANSACTION_waitFor, _data, _reply, 0);
          _reply.readException();
          _result = _reply.readInt();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public int exitValue() throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        int _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          boolean _status = mRemote.transact(Stub.TRANSACTION_exitValue, _data, _reply, 0);
          _reply.readException();
          _result = _reply.readInt();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public void destroy() throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          boolean _status = mRemote.transact(Stub.TRANSACTION_destroy, _data, _reply, 0);
          _reply.readException();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
      }
      @Override public boolean alive() throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        boolean _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          boolean _status = mRemote.transact(Stub.TRANSACTION_alive, _data, _reply, 0);
          _reply.readException();
          _result = (0!=_reply.readInt());
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public boolean waitForTimeout(long timeout, java.lang.String unit) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        boolean _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeLong(timeout);
          _data.writeString(unit);
          boolean _status = mRemote.transact(Stub.TRANSACTION_waitForTimeout, _data, _reply, 0);
          _reply.readException();
          _result = (0!=_reply.readInt());
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
    }
    static final int TRANSACTION_getOutputStream = (android.os.IBinder.FIRST_CALL_TRANSACTION + 0);
    static final int TRANSACTION_getInputStream = (android.os.IBinder.FIRST_CALL_TRANSACTION + 1);
    static final int TRANSACTION_getErrorStream = (android.os.IBinder.FIRST_CALL_TRANSACTION + 2);
    static final int TRANSACTION_waitFor = (android.os.IBinder.FIRST_CALL_TRANSACTION + 3);
    static final int TRANSACTION_exitValue = (android.os.IBinder.FIRST_CALL_TRANSACTION + 4);
    static final int TRANSACTION_destroy = (android.os.IBinder.FIRST_CALL_TRANSACTION + 5);
    static final int TRANSACTION_alive = (android.os.IBinder.FIRST_CALL_TRANSACTION + 6);
    static final int TRANSACTION_waitForTimeout = (android.os.IBinder.FIRST_CALL_TRANSACTION + 7);
  }
  /** @hide */
  public static final java.lang.String DESCRIPTOR = "moe.shizuku.server.IRemoteProcess";
  public android.os.ParcelFileDescriptor getOutputStream() throws android.os.RemoteException;
  public android.os.ParcelFileDescriptor getInputStream() throws android.os.RemoteException;
  public android.os.ParcelFileDescriptor getErrorStream() throws android.os.RemoteException;
  public int waitFor() throws android.os.RemoteException;
  public int exitValue() throws android.os.RemoteException;
  public void destroy() throws android.os.RemoteException;
  public boolean alive() throws android.os.RemoteException;
  public boolean waitForTimeout(long timeout, java.lang.String unit) throws android.os.RemoteException;
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
